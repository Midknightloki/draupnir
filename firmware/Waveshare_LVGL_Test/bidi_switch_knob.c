/*
 * SPDX-FileCopyrightText: 2016-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 *
 *
 * Modified by planevina 2025-01-20
 */

#include <stdio.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bidi_switch_knob.h"

static const char *TAG = "Knob";

#define TICKS_INTERVAL 3
#define DEBOUNCE_TICKS 2

#define KNOB_CHECK(a, str, ret_val)                               \
    if (!(a))                                                     \
    {                                                             \
        ESP_LOGE(TAG, "%s(%d): %s", __FUNCTION__, __LINE__, str); \
        return (ret_val);                                         \
    }

#define KNOB_CHECK_GOTO(a, str, label)                                         \
    if (!(a))                                                                  \
    {                                                                          \
        ESP_LOGE(TAG, "%s:%d (%s):%s", __FILE__, __LINE__, __FUNCTION__, str); \
        goto label;                                                            \
    }

#define CALL_EVENT_CB(ev) \
    if (knob->cb[ev])     \
    knob->cb[ev](knob, knob->usr_data[ev])

typedef struct Knob
{
    bool encoder_a_change;                          /*<! true means Encoder A phase Inverted*/
    bool encoder_b_change;                          /*<! true means Encoder B phase Inverted*/
    uint8_t debounce_a_cnt;                         /*!< Encoder A phase debounce count */
    uint8_t debounce_b_cnt;                         /*!< Encoder B phase debounce count */
    uint8_t encoder_a_level;                        /*!< Encoder A phase current level */
    uint8_t encoder_b_level;                        /*!< Encoder B phase current Level */
    knob_event_t event;                             /*!< Current event */
    int count_value;                                /*!< Knob count */
    uint8_t (*hal_knob_level)(void *hardware_data); /*!< Get current level */
    void *encoder_a;                                /*!< Encoder A phase gpio number */
    void *encoder_b;                                /*!< Encoder B phase gpio number */
    void *usr_data[KNOB_EVENT_MAX];                 /*!< User data for event */
    knob_cb_t cb[KNOB_EVENT_MAX];                   /*!< Event callback */
    struct Knob *next;                              /*!< Next pointer */
} knob_dev_t;

static knob_dev_t *s_head_handle = NULL;
static esp_timer_handle_t s_knob_timer_handle;
static bool s_is_timer_running = false;

// --- Diagnostic raw-pin ring buffer -----------------------------------------------------
//
// Records the packed (A,B) pin state every time it changes, independent of and without
// touching the decode/debounce logic above. Single-producer (esp_timer task, via
// knob_handler()) / single-consumer (loop task, via knob_debug_pop()) ring buffer. Capacity
// is a power of two so head/tail can be masked instead of compared, which is what makes this
// safe without a critical section: each side only ever advances its own index.
//
// On overflow the newest sample is dropped (not the oldest) and a sticky flag is set --
// losing the oldest samples would destroy exactly the transition sequence we're trying to
// read.
#define KNOB_DEBUG_RING_CAP 128
#define KNOB_DEBUG_RING_MASK (KNOB_DEBUG_RING_CAP - 1)

typedef struct
{
    uint8_t state;
    uint32_t t_ms;
} knob_debug_sample_t;

static knob_debug_sample_t s_debug_ring[KNOB_DEBUG_RING_CAP];
static volatile uint32_t s_debug_head = 0; /*!< next slot to write (producer-owned) */
static volatile uint32_t s_debug_tail = 0; /*!< next slot to read (consumer-owned) */
static volatile uint8_t s_debug_overflow = 0;
static uint8_t s_debug_last_state = 0xFF; /*!< sentinel: no sample recorded yet */

static void knob_debug_record(uint8_t pha_value, uint8_t phb_value)
{
    uint8_t state = (uint8_t)(((pha_value & 1) << 1) | (phb_value & 1));
    if (state == s_debug_last_state)
    {
        return;
    }
    s_debug_last_state = state;

    uint32_t head = s_debug_head;
    uint32_t next_head = head + 1;
    if ((next_head - s_debug_tail) > KNOB_DEBUG_RING_CAP)
    {
        /* Buffer full: drop this newest sample, keep the unread history. */
        s_debug_overflow = 1;
        return;
    }

    s_debug_ring[head & KNOB_DEBUG_RING_MASK].state = state;
    s_debug_ring[head & KNOB_DEBUG_RING_MASK].t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_debug_head = next_head;
}

int knob_debug_pop(uint8_t *state, uint32_t *t_ms)
{
    uint32_t tail = s_debug_tail;
    if (tail == s_debug_head)
    {
        return 0;
    }
    knob_debug_sample_t sample = s_debug_ring[tail & KNOB_DEBUG_RING_MASK];
    s_debug_tail = tail + 1;
    if (state)
    {
        *state = sample.state;
    }
    if (t_ms)
    {
        *t_ms = sample.t_ms;
    }
    return 1;
}

int knob_debug_overflowed(void)
{
    uint8_t was = s_debug_overflow;
    s_debug_overflow = 0;
    return was;
}
// --- End diagnostic raw-pin ring buffer -------------------------------------------------

// 判定函数
static void process_knob_channel(uint8_t current_level, uint8_t *prev_level,
                                 uint8_t *debounce_cnt, int *count_value,
                                 knob_event_t event, bool is_increment, knob_dev_t *knob)
{
    if (current_level == 0)
    {
        if (current_level != *prev_level)
            *debounce_cnt = 0;
        else if (*debounce_cnt < 255)
            /* Saturate, don't wrap. debounce_cnt is uint8_t and ticks once per
             * TICKS_INTERVAL (3 ms) poll while the contact is held low. Left to
             * wrap, 256 samples = 768 ms of held contact rolls it back to 0, so
             * a long-held click makes the release edge's ++(*debounce_cnt) >=
             * DEBOUNCE_TICKS test read 1 >= 2 (false) instead of true, and the
             * click is silently dropped. Measured on hardware via the raw-pin
             * capture: A=1 B=0 at t=29546 -> A=1 B=1 at t=30314 (768 ms low)
             * produced no knob event. Saturating is safe because debounce_cnt
             * is only ever compared against DEBOUNCE_TICKS (2); anything at or
             * above that threshold behaves identically. */
            (*debounce_cnt)++;
    }
    else
    {
        if (current_level != *prev_level && ++(*debounce_cnt) >= DEBOUNCE_TICKS)
        {
            *debounce_cnt = 0;
            *count_value += is_increment ? 1 : -1;
            knob->event = event;
            CALL_EVENT_CB(event);
        }
        else
            *debounce_cnt = 0;
    }
    *prev_level = current_level;
}

static void knob_handler(knob_dev_t *knob)
{
    uint8_t pha_value = knob->hal_knob_level(knob->encoder_a);
    uint8_t phb_value = knob->hal_knob_level(knob->encoder_b);

    knob_debug_record(pha_value, phb_value);

    process_knob_channel(pha_value, &knob->encoder_a_level,
                         &knob->debounce_a_cnt, &knob->count_value,
                         KNOB_RIGHT, true, knob);

    process_knob_channel(phb_value, &knob->encoder_b_level,
                         &knob->debounce_b_cnt, &knob->count_value,
                         KNOB_LEFT, false, knob);
}

// 这是timer的回调函数，定期执行
static void knob_cb(void *args)
{
    knob_dev_t *target;
    for (target = s_head_handle; target; target = target->next)
    {
        knob_handler(target);
    }
}

knob_handle_t iot_knob_create(const knob_config_t *config)
{
    KNOB_CHECK(NULL != config, "config pointer can't be NULL!", NULL)
    KNOB_CHECK(config->gpio_encoder_a != config->gpio_encoder_b, "encoder A can't be the same as encoder B", NULL);

    knob_dev_t *knob = (knob_dev_t *)calloc(1, sizeof(knob_dev_t));
    KNOB_CHECK(NULL != knob, "alloc knob failed", NULL);

    esp_err_t ret = ESP_OK;
    ret = knob_gpio_init(config->gpio_encoder_a);
    KNOB_CHECK(ESP_OK == ret, "encoder A gpio init failed", NULL);
    ret = knob_gpio_init(config->gpio_encoder_b);
    KNOB_CHECK_GOTO(ESP_OK == ret, "encoder B gpio init failed", _encoder_deinit);

    knob->hal_knob_level = knob_gpio_get_key_level;
    knob->encoder_a = (void *)(long)config->gpio_encoder_a;
    knob->encoder_b = (void *)(long)config->gpio_encoder_b;

    knob->encoder_a_level = knob->hal_knob_level(knob->encoder_a);
    knob->encoder_b_level = knob->hal_knob_level(knob->encoder_b);

    knob->event = KNOB_NONE;

    knob->next = s_head_handle;
    s_head_handle = knob;

    if (!s_knob_timer_handle)
    {
        esp_timer_create_args_t knob_timer = {0};
        knob_timer.arg = NULL;
        knob_timer.callback = knob_cb;
        knob_timer.dispatch_method = ESP_TIMER_TASK;
        knob_timer.name = "knob_timer";
        esp_timer_create(&knob_timer, &s_knob_timer_handle);
    }

    if (!s_is_timer_running)
    {
        esp_timer_start_periodic(s_knob_timer_handle, TICKS_INTERVAL * 1000U);
        s_is_timer_running = true;
    }

    ESP_LOGI(TAG, "Iot Knob Config Succeed, encoder A:%d, encoder B:%d", config->gpio_encoder_a, config->gpio_encoder_b);
    return (knob_handle_t)knob;

_encoder_deinit:
    knob_gpio_deinit(config->gpio_encoder_b);
    knob_gpio_deinit(config->gpio_encoder_a);
    return NULL;
}

esp_err_t iot_knob_delete(knob_handle_t knob_handle)
{
    esp_err_t ret = ESP_OK;
    KNOB_CHECK(NULL != knob_handle, "Pointer of handle is invalid", ESP_ERR_INVALID_ARG);
    knob_dev_t *knob = (knob_dev_t *)knob_handle;
    ret = knob_gpio_deinit((int)(knob->usr_data));
    KNOB_CHECK(ESP_OK == ret, "knob deinit failed", ESP_FAIL);
    knob_dev_t **curr;
    for (curr = &s_head_handle; *curr;)
    {
        knob_dev_t *entry = *curr;
        if (entry == knob)
        {
            *curr = entry->next;
            free(entry);
        }
        else
        {
            curr = &entry->next;
        }
    }

    uint16_t number = 0;
    knob_dev_t *target = s_head_handle;
    while (target)
    {
        target = target->next;
        number++;
    }
    ESP_LOGD(TAG, "remain knob number=%d", number);

    if (0 == number && s_is_timer_running)
    {
        esp_timer_stop(s_knob_timer_handle);
        esp_timer_delete(s_knob_timer_handle);
        s_is_timer_running = false;
    }

    return ESP_OK;
}

esp_err_t iot_knob_register_cb(knob_handle_t knob_handle, knob_event_t event, knob_cb_t cb, void *usr_data)
{
    KNOB_CHECK(NULL != knob_handle, "Pointer of handle is invalid", ESP_ERR_INVALID_ARG);
    KNOB_CHECK(event < KNOB_EVENT_MAX, "event is invalid", ESP_ERR_INVALID_ARG);
    knob_dev_t *knob = (knob_dev_t *)knob_handle;
    knob->cb[event] = cb;
    knob->usr_data[event] = usr_data;
    return ESP_OK;
}

esp_err_t iot_knob_unregister_cb(knob_handle_t knob_handle, knob_event_t event)
{
    KNOB_CHECK(NULL != knob_handle, "Pointer of handle is invalid", ESP_ERR_INVALID_ARG);
    KNOB_CHECK(event < KNOB_EVENT_MAX, "event is invalid", ESP_ERR_INVALID_ARG);
    knob_dev_t *knob = (knob_dev_t *)knob_handle;
    knob->cb[event] = NULL;
    knob->usr_data[event] = NULL;
    return ESP_OK;
}

knob_event_t iot_knob_get_event(knob_handle_t knob_handle)
{
    KNOB_CHECK(NULL != knob_handle, "Pointer of handle is invalid", ESP_ERR_INVALID_ARG);
    knob_dev_t *knob = (knob_dev_t *)knob_handle;
    return knob->event;
}

int iot_knob_get_count_value(knob_handle_t knob_handle)
{
    KNOB_CHECK(NULL != knob_handle, "Pointer of handle is invalid", ESP_ERR_INVALID_ARG);
    knob_dev_t *knob = (knob_dev_t *)knob_handle;
    return knob->count_value;
}

esp_err_t iot_knob_clear_count_value(knob_handle_t knob_handle)
{
    KNOB_CHECK(NULL != knob_handle, "Pointer of handle is invalid", ESP_ERR_INVALID_ARG);
    knob_dev_t *knob = (knob_dev_t *)knob_handle;
    knob->count_value = 0;
    return ESP_OK;
}

esp_err_t iot_knob_resume(void)
{
    KNOB_CHECK(s_knob_timer_handle, "knob timer handle is invalid", ESP_ERR_INVALID_STATE);
    KNOB_CHECK(!s_is_timer_running, "knob timer is already running", ESP_ERR_INVALID_STATE);

    esp_err_t err = esp_timer_start_periodic(s_knob_timer_handle, TICKS_INTERVAL * 1000U);
    KNOB_CHECK(ESP_OK == err, "knob timer start failed", ESP_FAIL);
    s_is_timer_running = true;
    return ESP_OK;
}

esp_err_t iot_knob_stop(void)
{
    KNOB_CHECK(s_knob_timer_handle, "knob timer handle is invalid", ESP_ERR_INVALID_STATE);
    KNOB_CHECK(s_is_timer_running, "knob timer is not running", ESP_ERR_INVALID_STATE);

    esp_err_t err = esp_timer_stop(s_knob_timer_handle);
    KNOB_CHECK(ESP_OK == err, "knob timer stop failed", ESP_FAIL);
    s_is_timer_running = false;
    return ESP_OK;
}

esp_err_t knob_gpio_init(uint32_t gpio_num)
{
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = 1,
    };
    esp_err_t ret = gpio_config(&gpio_cfg);

    return ret;
}

esp_err_t knob_gpio_deinit(uint32_t gpio_num)
{
    return gpio_reset_pin(gpio_num);
}

uint8_t knob_gpio_get_key_level(void *gpio_num)
{
    return (uint8_t)gpio_get_level((uint32_t)gpio_num);
}
