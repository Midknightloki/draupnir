#include <math.h>
#include "USB.h"
#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "lcd_config.h"
#include "bidi_switch_knob.h"
#include "macro_engine.h"
#include "ble_engine.h"
#include "haptics.h"

#define ENCODER_ECA_PIN 8
#define ENCODER_ECB_PIN 7

// Torus (donut) ring, dynamically divided into one wedge per ACTIVE macro (up to
// NUM_MACRO_SLOTS) instead of always reserving all 16 slots -- fewer macros means fewer,
// bigger wedges instead of wasted screen space on empty ones.
#define RING_OUTER_R 172
#define RING_INNER_R 92
#define RING_MID_R ((RING_OUTER_R + RING_INNER_R) / 2)
#define WEDGE_GAP_DEG 2.0f

// active_positions[v] = the profiles.json "pos" (0-15) shown at wedge v. selected_idx and all
// wedge/label indices below are in terms of v (0..active_count-1), not raw pos -- macros_fire()
// and profiles_find_macro() still take a real pos, so callers go through active_positions[].
static int active_positions[NUM_MACRO_SLOTS];
static int active_count = 0;

static lv_obj_t *macro_labels[NUM_MACRO_SLOTS];
static int selected_idx = 0;
static knob_handle_t s_knob = NULL;
static EventGroupHandle_t knob_events = NULL;
static lv_obj_t *center_label;

static uint32_t parse_hex_color(const char *hex, uint32_t fallback) {
  if (hex == nullptr || strlen(hex) < 6) return fallback;
  int offset = (hex[0] == '#') ? 1 : 0;
  return (uint32_t)strtol(hex + offset, nullptr, 16) & 0xFFFFFF;
}

#define EMPTY_SLOT_COLOR 0x242430

// The running indicator modulates the wedge's BRIGHTNESS rather than tinting it a fixed colour.
//
// A fixed hue cannot work here: macro colours are user-chosen, so any constant we pick is
// invisible on a macro someone coloured the same. That is not hypothetical -- the first version
// used a green pulse and was completely invisible on a green macro, while showing correctly on a
// blue one. Picking a different constant just moves the collision to a different user.
//
// Overlaying white for half the cycle and black for the other half makes the wedge breathe
// lighter-then-darker, which is visible against ANY colour including white and black. It also
// stops competing with the white selection outline: "selected" is a static shape, "running" is
// motion, so the two never read as the same state.
#define PULSE_PERIOD_MS 1400
#define PULSE_PEAK_OPA 110

// Sets *lighten and returns the overlay opacity for this instant. Ramps up and back down within
// each half-cycle so it eases rather than snapping at the direction change.
static lv_opa_t pulse_overlay(bool *lighten) {
  uint32_t phase = millis() % PULSE_PERIOD_MS;
  uint32_t half = PULSE_PERIOD_MS / 2;
  *lighten = (phase < half);
  uint32_t t = *lighten ? phase : (phase - half); // 0..half within the current direction
  uint32_t quarter = half / 2;
  uint32_t tri = (t < quarter) ? t : (half - t);  // 0..quarter, up then back down
  return (lv_opa_t)((tri * PULSE_PEAK_OPA) / quarter);
}

static void scan_active_positions(void) {
  active_count = 0;
  for (int i = 0; i < NUM_MACRO_SLOTS; i++) {
    if (!profiles_find_macro(i).isNull()) active_positions[active_count++] = i;
  }
}

// Center angle (in lv_draw_arc's own angle units) of wedge v, given `count` total wedges.
// Verified directly against lv_arc.c's own knob-placement code (the one part of LVGL
// guaranteed to use its angle units correctly): knob_x = R*sin(angle+90) = R*cos(angle),
// knob_y = R*sin(angle) -- i.e. plain x=R*cos(angle), y=R*sin(angle), which puts 0deg at
// 3 o'clock and makes increasing angle go CLOCKWISE on screen. The +270 offset rotates that
// so v=0 lands at 12 o'clock; since both v and the angle unit increase clockwise, no direction
// flip is needed between them.
static float wedge_center_angle(int v, int count) {
  float wedge_deg = 360.0f / count;
  float a = fmodf(v * wedge_deg + 270.0f, 360.0f);
  if (a < 0.0f) a += 360.0f;
  return a;
}

// Rebuilds the label objects for the currently active macros. Safe to call again later (once
// profile edits arrive over BLE) -- deletes any previously-created labels first.
static void rebuild_ring_layout(void) {
  for (int i = 0; i < NUM_MACRO_SLOTS; i++) {
    if (macro_labels[i]) {
      lv_obj_del(macro_labels[i]);
      macro_labels[i] = NULL;
    }
  }

  scan_active_positions();
  if (active_count == 0) {
    lv_label_set_text(center_label, "(no macros)");
    return;
  }

  float wedge_deg = 360.0f / active_count;
  float half_span = wedge_deg / 2.0f - WEDGE_GAP_DEG / 2.0f;
  // Chord width available at the wedge's mid-radius, so text wraps to what's actually there
  // instead of a fixed guess -- this is what was overflowing before at 16 fixed slots.
  float chord = 2.0f * RING_MID_R * sinf(half_span * (float)M_PI / 180.0f) - 6.0f;
  if (chord < 20.0f) chord = 20.0f;

  lv_obj_t *scr = lv_scr_act();
  for (int v = 0; v < active_count; v++) {
    JsonObject macro = profiles_find_macro(active_positions[v]);
    float angle = wedge_center_angle(v, active_count);
    float rad = angle * (float)M_PI / 180.0f;
    float x = EXAMPLE_LCD_H_RES / 2.0f + RING_MID_R * cosf(rad);
    float y = EXAMPLE_LCD_V_RES / 2.0f + RING_MID_R * sinf(rad);

    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_width(label, (lv_coord_t)chord);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT); // single line, truncates with "..." -- never overflows vertically
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, macro.isNull() ? "" : (const char *)(macro["name"] | "?"));
    lv_obj_set_pos(label, (lv_coord_t)(x - chord / 2.0f), (lv_coord_t)(y - 9));

    macro_labels[v] = label;
  }
  Serial.printf("[diag] rebuild_ring_layout: active_count=%d\n", active_count);
}

// Draws the donut wedges directly on the screen every repaint (selection change, layout
// rebuild, etc.) -- LVGL has no built-in clickable "pie slice" widget, so this hand-draws with
// lv_draw_arc (the same primitive lv_arc uses internally) instead of per-wedge button objects.
// Hooked on LV_EVENT_DRAW_MAIN_END so it paints after the screen's own bg fill but before its
// children (the labels) draw, so label text lands on top of the wedges.
static void ring_draw_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN_END) return;
  if (active_count == 0) return;
  lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
  lv_point_t center_pt = { EXAMPLE_LCD_H_RES / 2, EXAMPLE_LCD_V_RES / 2 };
  float wedge_deg = 360.0f / active_count;
  float half_span = wedge_deg / 2.0f - WEDGE_GAP_DEG / 2.0f;

  for (int v = 0; v < active_count; v++) {
    float center = wedge_center_angle(v, active_count);
    float start = center - half_span;
    if (start < 0.0f) start += 360.0f;
    float end = start + (wedge_deg - WEDGE_GAP_DEG);

    JsonObject macro = profiles_find_macro(active_positions[v]);
    uint32_t color = macro.isNull() ? (uint32_t)EMPTY_SLOT_COLOR
                                     : parse_hex_color(macro["color"] | "#FFFFFF", 0xFFFFFF);

    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = lv_color_hex(color);
    dsc.width = RING_OUTER_R - RING_INNER_R;
    lv_draw_arc(draw_ctx, &dsc, &center_pt, RING_OUTER_R, (uint16_t)lroundf(start), (uint16_t)lroundf(end));

    if (v == selected_idx) {
      lv_draw_arc_dsc_t hl;
      lv_draw_arc_dsc_init(&hl);
      hl.color = lv_color_white();
      hl.width = 4;
      lv_draw_arc(draw_ctx, &hl, &center_pt, RING_OUTER_R, (uint16_t)lroundf(start), (uint16_t)lroundf(end));
      lv_draw_arc(draw_ctx, &hl, &center_pt, RING_INNER_R + 4, (uint16_t)lroundf(start), (uint16_t)lroundf(end));
    }

    // Drawn last so it wins over the white selection highlight when a wedge is both selected and
    // running -- "running" is the more urgent state, and a macro looping unnoticed is exactly the
    // failure this indicator exists to prevent.
    if (macros_is_running(active_positions[v])) {
      bool lighten = true;
      lv_opa_t opa = pulse_overlay(&lighten);
      lv_draw_arc_dsc_t run;
      lv_draw_arc_dsc_init(&run);
      run.color = lighten ? lv_color_white() : lv_color_black();
      run.opa = opa;
      run.width = RING_OUTER_R - RING_INNER_R; // whole band, so the wedge itself breathes
      lv_draw_arc(draw_ctx, &run, &center_pt, RING_OUTER_R, (uint16_t)lroundf(start), (uint16_t)lroundf(end));
    }
  }
}

static void update_center_label(int idx) {
  if (active_count == 0) return;
  JsonObject macro = profiles_find_macro(active_positions[idx]);
  lv_label_set_text(center_label, macro.isNull() ? "?" : (const char *)(macro["name"] | "?"));
}

static void select_idx(int idx) {
  selected_idx = idx;
  update_center_label(selected_idx);
  lv_obj_invalidate(lv_scr_act());
}

// Inverse of wedge_center_angle() -- maps a tap point to a wedge index using the same angle
// convention (atan2f already returns angle in exactly lv_draw_arc's units, since that
// convention IS the standard x=R*cos/y=R*sin parametrization; see wedge_center_angle's comment).
static int wedge_index_from_point(lv_coord_t px, lv_coord_t py) {
  if (active_count == 0) return 0;
  float dx = (float)px - EXAMPLE_LCD_H_RES / 2.0f;
  float dy = (float)py - EXAMPLE_LCD_V_RES / 2.0f;
  float angle = atan2f(dy, dx) * 180.0f / (float)M_PI;
  if (angle < 0.0f) angle += 360.0f;
  float wedge_deg = 360.0f / active_count;
  int idx = (int)lroundf((angle - 270.0f) / wedge_deg);
  return ((idx % active_count) + active_count) % active_count;
}

// A tap inside the inner radius (the center label area) fires whatever the encoder currently
// has selected, without changing the selection. A tap on the ring itself selects and fires
// that wedge. Taps outside the outer radius (round-glass bezel) are ignored.
static void screen_click_cb(lv_event_t *e) {
  if (active_count == 0) return;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);

  float dx = (float)p.x - EXAMPLE_LCD_H_RES / 2.0f;
  float dy = (float)p.y - EXAMPLE_LCD_V_RES / 2.0f;
  float dist = sqrtf(dx * dx + dy * dy);

  // DIAGNOSTIC (temporary): the touch->fire path had no logging at all, so a tap that never
  // fired was indistinguishable from a tap that never arrived. Logs whether CLICKED reaches us,
  // where, and which branch it takes.
  Serial.printf("[tap] CLICKED x=%d y=%d dist=%.1f inner=%d outer=%d active_count=%d\n",
                (int)p.x, (int)p.y, dist, RING_INNER_R, RING_OUTER_R, active_count);

  if (dist < RING_INNER_R) {
    Serial.printf("[tap] -> center, fire pos=%d\n", active_positions[selected_idx]);
    macros_request_fire(active_positions[selected_idx]);
  } else if (dist <= RING_OUTER_R + 10) {
    int idx = wedge_index_from_point(p.x, p.y);
    Serial.printf("[tap] -> wedge v=%d fire pos=%d\n", idx, active_positions[idx]);
    select_idx(idx);
    macros_request_fire(active_positions[idx]);
  } else {
    Serial.println("[tap] -> outside ring, ignored");
  }
}

// Swipe down anywhere = stop every running macro. A gesture rather than an on-screen button
// because it can be performed without looking at the screen -- the same reasoning the M5Dial
// build used -- which is what you want when a runaway "toggle" macro is spamming the host and
// the screen is the last thing you're looking at.
//
// Runs on the LVGL task, so it must NOT call macros_stop_all() directly (see macro_engine.h);
// macros_request_stop_all() queues it for loop().
static void screen_gesture_cb(lv_event_t *e) {
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  // DIAGNOSTIC (temporary): a GESTURE firing on ordinary taps would explain taps not reaching
  // CLICKED at all -- LVGL delivers one or the other, not both.
  Serial.printf("[tap] GESTURE dir=%d (bottom=%d) any_running=%d\n",
                (int)dir, (int)LV_DIR_BOTTOM, (int)macros_any_running());
  if (dir != LV_DIR_BOTTOM) return;
  if (!macros_any_running()) return;

  Serial.println("[diag] swipe down -> kill all macros");
  macros_request_stop_all();
  haptics_pulse(); // replaces the M5Dial's buzzer blip: eyes-free confirmation the gesture took

  // Suppress the rest of this touch so the release does not also land as a CLICKED event on
  // whatever wedge the finger happens to end over -- otherwise killing everything could
  // immediately re-fire a macro.
  lv_indev_wait_release(indev);
}

// The running pulse is time-driven, so something has to repaint while a macro runs. Done here
// from loop() under lvgl_lock(), matching update_pairing_overlay()/update_profiles_reload()
// rather than an lv_timer -- an lv_timer runs on the LVGL task, which would read runningMacros[]
// concurrently with loop()'s writes to it.
static unsigned long last_pulse_repaint = 0;
static void update_running_pulse(void) {
  static bool was_running = false;
  bool running = macros_any_running();

  if (!running) {
    if (was_running) {
      was_running = false;
      // One final repaint on the running->idle edge, or the last-drawn pulse frame stays burned
      // on screen and the ring looks permanently "running".
      if (lvgl_lock(50)) {
        lv_obj_invalidate(lv_scr_act());
        lvgl_unlock();
      }
    }
    return;
  }

  was_running = true;
  unsigned long now = millis();
  if (now - last_pulse_repaint < 60) return; // ~16 fps: smooth enough for a 1.2 s pulse, cheap
  last_pulse_repaint = now;
  if (lvgl_lock(50)) {
    lv_obj_invalidate(lv_scr_act());
    lvgl_unlock();
  }
}

// Full-screen overlay shown while ble_pairing_active() is true, on top of the ring (created
// after it, so it draws later/on top in LVGL's default per-screen z-order). Hidden the rest of
// the time.
static lv_obj_t *pairing_overlay = nullptr;
static lv_obj_t *pairing_label = nullptr;

static void build_pairing_overlay(void) {
  lv_obj_t *scr = lv_scr_act();
  pairing_overlay = lv_obj_create(scr);
  lv_obj_set_size(pairing_overlay, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
  lv_obj_set_pos(pairing_overlay, 0, 0);
  lv_obj_set_style_bg_color(pairing_overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(pairing_overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pairing_overlay, 0, 0);
  lv_obj_set_style_border_width(pairing_overlay, 0, 0);
  lv_obj_clear_flag(pairing_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(pairing_overlay, LV_OBJ_FLAG_HIDDEN);

  pairing_label = lv_label_create(pairing_overlay);
  lv_obj_set_style_text_color(pairing_label, lv_color_white(), 0);
  lv_obj_set_style_text_align(pairing_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(pairing_label, "Pair with app,\nenter PIN:\n000000");
  lv_obj_center(pairing_label);
}

// Polled from loop() (ble_engine's callbacks run on the Bluedroid task, not loop() -- LVGL
// objects are only ever touched here, under lvgl_lock(), matching the pattern already used for
// knob input in encoder_task()).
static void update_pairing_overlay(void) {
  static bool wasPairing = false;
  bool isPairing = ble_pairing_active();
  if (isPairing == wasPairing) return;
  // Acquire the lock BEFORE committing wasPairing. Committing first meant a single 100ms lock
  // timeout consumed the transition permanently -- the overlay then never appeared, the passkey
  // was never displayed, and pairing became impossible. Leaving wasPairing alone on lock failure
  // means the next loop() tick simply retries.
  if (!lvgl_lock(100)) return;
  if (isPairing) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Pair with app,\nenter PIN:\n%06u", (unsigned)ble_passkey());
    lv_label_set_text(pairing_label, buf);
    lv_obj_clear_flag(pairing_overlay, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(pairing_overlay, LV_OBJ_FLAG_HIDDEN);
  }
  lvgl_unlock();
  wasPairing = isPairing; // only now has the transition actually been applied to the UI
}

// Polled from loop() -- a save_profiles command (handled entirely on the Bluedroid/NimBLE host
// task, see ble_engine.h) sets ble_profiles_dirty() once it's written+reloaded a new
// profiles.json. Rebuilding the ring here, under lvgl_lock(), keeps every LVGL touch on either
// loop() or the LVGL task itself, same pattern as update_pairing_overlay()/encoder_task().
static void update_profiles_reload(void) {
  if (!ble_profiles_dirty()) return;
  if (!lvgl_lock(200)) return;
  Serial.println("[diag] profiles changed via BLE, rebuilding ring UI");
  rebuild_ring_layout();
  selected_idx = 0;
  if (active_count > 0) select_idx(selected_idx);
  lvgl_unlock();
  ble_clear_profiles_dirty();
}

static void build_ring_ui(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_add_event_cb(scr, ring_draw_event_cb, LV_EVENT_DRAW_MAIN_END, NULL);
  lv_obj_add_event_cb(scr, screen_click_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

  center_label = lv_label_create(scr);
  lv_obj_set_style_text_color(center_label, lv_color_white(), 0);
  lv_obj_center(center_label);

  rebuild_ring_layout();
  if (active_count > 0) select_idx(selected_idx);

  build_pairing_overlay();
}

static void _knob_left_cb(void *arg, void *data) {
  xEventGroupSetBits(knob_events, (1 << 0));
}
static void _knob_right_cb(void *arg, void *data) {
  xEventGroupSetBits(knob_events, (1 << 1));
}

static void encoder_task(void *arg) {
  for (;;) {
    EventBits_t bits = xEventGroupWaitBits(knob_events, 0x03, pdTRUE, pdFALSE, portMAX_DELAY);
    int delta = 0;
    if (bits & (1 << 0)) delta -= 1;
    if (bits & (1 << 1)) delta += 1;
    if (delta != 0 && active_count > 0 && lvgl_lock(100)) {
      select_idx((selected_idx + delta + active_count) % active_count);
      lvgl_unlock();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("[diag] setup start");
  hid_init();
  Serial.println("[diag] hid_init done");
  USB.begin();
  Serial.println("[diag] USB.begin done");
  delay(200); // let USB settle before BLE init -- BLE coming up right after USB.begin() has
              // caused issues on the M5Dial reference firmware if the order is reversed
  ble_init();
  Serial.println("[diag] ble_init done");

  profiles_init();
  Serial.printf("[diag] profiles_init done heap=%u\n", ESP.getFreeHeap());

  Touch_Init();
  Serial.println("[diag] Touch_Init done");
  // DIAGNOSTIC BISECT (temporary): touch stopped reaching LVGL entirely after this was added --
  // no CLICKED and no GESTURE events, while the encoder kept working. haptics_init() is the only
  // new code sharing I2C bus 0 with the CST816 touch controller at 0x15, and it probes all 112
  // addresses at boot before lcd_lvgl_Init() registers the touch input device. Disabled to test
  // that as a single variable; everything else on this branch stays in.
  // haptics_init();
  Serial.println("[diag] haptics_init SKIPPED (bisect: testing I2C interference with touch)");


  lcd_lvgl_Init();
  Serial.printf("[diag] lcd_lvgl_Init done heap=%u\n", ESP.getFreeHeap());
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
  Serial.println("[diag] backlight init done");

  if (lvgl_lock(-1)) {
    Serial.println("[diag] lvgl locked, building ring ui");
    build_ring_ui();
    Serial.println("[diag] build_ring_ui returned");
    lvgl_unlock();
  } else {
    Serial.println("[diag] FAILED to acquire lvgl lock");
  }

  knob_events = xEventGroupCreate();
  knob_config_t cfg = {
    .gpio_encoder_a = ENCODER_ECA_PIN,
    .gpio_encoder_b = ENCODER_ECB_PIN,
  };
  s_knob = iot_knob_create(&cfg);
  if (s_knob == NULL) {
    Serial.println("knob create failed");
  } else {
    iot_knob_register_cb(s_knob, KNOB_LEFT, _knob_left_cb, NULL);
    iot_knob_register_cb(s_knob, KNOB_RIGHT, _knob_right_cb, NULL);
  }
  xTaskCreate(encoder_task, "encoder_task", 3000, NULL, 2, NULL);
  Serial.println("[diag] setup complete, entering loop");
}

static unsigned long last_loop_print = 0;
void loop() {
  macros_update();
  ble_update();
  update_pairing_overlay();
  update_profiles_reload();
  update_running_pulse();
  if (millis() - last_loop_print > 3000) {
    last_loop_print = millis();
    // any_running is the important new field: a "toggle" macro never terminates, so one left
    // looping would drive update_running_pulse()'s 16fps full-ring repaint forever and could
    // starve the LVGL task of the lock it needs to process touch.
    Serial.printf("[diag] loop alive heap=%u any_running=%d active_count=%d selected=%d\n",
                  ESP.getFreeHeap(), (int)macros_any_running(), active_count, selected_idx);
  }
  delay(5);
}
