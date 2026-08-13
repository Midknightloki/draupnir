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

// A detented encoder completes one full quadrature cycle per click = 4 transitions.
// If hardware testing shows one physical click producing two events, this is too low;
// if it shows every other click doing nothing, it is too high. It is the one number to
// adjust, and the symptom tells you which way.
#define KNOB_COUNTS_PER_DETENT 4

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
    uint8_t quad_state;                             /*!< Previous quadrature state, (A<<1)|B */
    int8_t quad_accum;                              /*!< Sub-detent transition accumulator; carries
                                                          the remainder across polls so a slow,
                                                          partial turn doesn't lose movement */
    knob_event_t event;                             /*!< Current event */
    int count_value;                                /*!< Knob count, in DETENTS (net movement),
                                                          not raw quadrature transitions */
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

// Quadrature transition table, indexed by (prev_state << 2) | curr_state where
// state = (A << 1) | B. One direction walks 00->01->11->10->00, the other reverses it.
// Entries that are 0 are either "no movement" or an ILLEGAL double transition -- the latter
// means a bounced or missed edge, and absorbing it is the whole point of decoding phase
// rather than counting edges per pin.
static const int8_t QUAD_TABLE[16] = {
   0, +1, -1,  0,
  -1,  0,  0, +1,
  +1,  0,  0, -1,
   0, -1, +1,  0,
};

static void knob_handler(knob_dev_t *knob)
{
    uint8_t pha_value = knob->hal_knob_level(knob->encoder_a);
    uint8_t phb_value = knob->hal_knob_level(knob->encoder_b);
    uint8_t curr_state = (uint8_t)((pha_value << 1) | phb_value);

    int8_t step = QUAD_TABLE[(knob->quad_state << 2) | curr_state];
    knob->quad_state = curr_state;

    if (step == 0)
    {
        // No movement, or an illegal double transition (bounce/missed sample) -- absorbed.
        return;
    }

    knob->quad_accum += step;

    if (knob->quad_accum >= KNOB_COUNTS_PER_DETENT)
    {
        knob->quad_accum -= KNOB_COUNTS_PER_DETENT; // carry remainder, don't zero it
        knob->count_value += 1;
        knob->event = KNOB_RIGHT;
        CALL_EVENT_CB(KNOB_RIGHT);
    }
    else if (knob->quad_accum <= -KNOB_COUNTS_PER_DETENT)
    {
        knob->quad_accum += KNOB_COUNTS_PER_DETENT; // carry remainder, don't zero it
        knob->count_value -= 1;
        knob->event = KNOB_LEFT;
        CALL_EVENT_CB(KNOB_LEFT);
    }
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

    {
        uint8_t pha_value = knob->hal_knob_level(knob->encoder_a);
        uint8_t phb_value = knob->hal_knob_level(knob->encoder_b);
        knob->quad_state = (uint8_t)((pha_value << 1) | phb_value);
    }
    knob->quad_accum = 0;

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
