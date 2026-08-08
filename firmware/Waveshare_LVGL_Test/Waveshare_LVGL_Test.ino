#include <math.h>
#include "USB.h"
#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "lcd_config.h"
#include "bidi_switch_knob.h"
#include "macro_engine.h"
#include "device_state.h"
#include "ble_engine.h"
#include "haptics.h"
#include "trace.h"

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
// Declared up here rather than beside the rest of the pairing UI below, because
// rebuild_ring_layout() has to re-assert its z-order after re-creating the wedge labels (see the
// comment there) and that function is defined earlier in this file.
static lv_obj_t *pairing_overlay = nullptr;

// ---- Settings menu ----------------------------------------------------------------------
// ui_mode is owned by loop(). LVGL-task callbacks (touch, gesture) and encoder_task only ever
// RAISE a request flag; loop() performs the transition under lvgl_lock(). Same pattern as
// update_pairing_overlay()/update_profiles_reload(). See spec section 9.
typedef enum { UI_RING, UI_SETTINGS_LIST, UI_SETTINGS_EDIT } ui_mode_t;
static ui_mode_t ui_mode = UI_RING;

static volatile bool settings_open_requested  = false;
static volatile bool settings_close_requested = false;
static volatile bool settings_enter_requested = false;

static lv_obj_t *settings_overlay = nullptr;
static lv_obj_t *settings_list_panel = nullptr;
static lv_obj_t *settings_edit_panel = nullptr;
static lv_obj_t *gauge_arc   = nullptr;
static lv_obj_t *gauge_value = nullptr;
static lv_obj_t *gauge_label = nullptr;
static lv_obj_t *settings_rows[4];          // sized for growth; SETTINGS_ITEM_COUNT is the truth
static int settings_sel = 0;
static unsigned long settings_last_activity = 0;

#define SETTINGS_IDLE_TIMEOUT_MS 8000
// Vertical distance between rows. The active row sits at the exact screen centre and the others
// are offset from it, so scrolling moves the whole column past a fixed centre -- the slot
// machine -- rather than moving a highlight down a static list.
#define SETTINGS_ROW_PITCH 54

// Backlight duty range. The floor is deliberately NOT 0: a knob that can be turned to a black
// screen looks bricked and leaves no way to find the setting again. Step 16 gives ~15 detents
// across the range. Duty is linear, so the low end feels perceptually coarse; gamma is a polish
// item, not this milestone.
#define BRIGHTNESS_MIN  20
#define BRIGHTNESS_MAX  255
#define BRIGHTNESS_STEP 16

static uint8_t current_duty = BRIGHTNESS_MAX;  // real value assigned in setup()
static bool    brightness_dirty = false;

static int  brightness_get(void) { return current_duty; }

// Live preview ONLY. NVS is written once, when Settings closes -- never per detent, and never
// from the LVGL task, where a flash write would stall the renderer.
static void brightness_apply(int duty) {
  if (duty < BRIGHTNESS_MIN) duty = BRIGHTNESS_MIN;
  if (duty > BRIGHTNESS_MAX) duty = BRIGHTNESS_MAX;
  current_duty = (uint8_t)duty;
  brightness_dirty = true;
  setUpdutySubdivide(current_duty);
}

// Settings are declared as data, not as bespoke screens, so an M10 item is one table entry.
// Every item is currently a knob-adjusted numeric value, so no kind discriminator is needed;
// a non-numeric setting (a buzzer on/off, say) will need a labels array added here.
typedef struct {
  const char *name;
  int         min, max, step;
  int  (*get)(void);
  void (*apply)(int);   // live preview while the knob turns
} setting_item_t;

static const setting_item_t SETTINGS_ITEMS[] = {
  { "Brightness", BRIGHTNESS_MIN, BRIGHTNESS_MAX, BRIGHTNESS_STEP, brightness_get, brightness_apply },
};
#define SETTINGS_ITEM_COUNT ((int)(sizeof(SETTINGS_ITEMS) / sizeof(SETTINGS_ITEMS[0])))

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
    lv_obj_set_style_text_font(label, &orbitron_12, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_width(label, (lv_coord_t)chord);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT); // single line, truncates with "..." -- never overflows vertically
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, macro.isNull() ? "" : (const char *)(macro["name"] | "?"));
    lv_obj_set_pos(label, (lv_coord_t)(x - chord / 2.0f), (lv_coord_t)(y - 9));

    macro_labels[v] = label;
  }
  // Re-assert overlay z-order after re-creating the wedge labels above, or one profile save is
  // enough to leave macro names bleeding through an overlay. Settings first, pairing last, so
  // pairing always wins.
  if (settings_overlay) lv_obj_move_foreground(settings_overlay);
  if (pairing_overlay)  lv_obj_move_foreground(pairing_overlay);

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
  // A tap while Settings is open activates the centred item. It must NEVER fall through to
  // macros_request_fire() -- firing a macro from a menu tap is the wrong surprise on a device
  // whose whole job is sending keystrokes.
  if (ui_mode != UI_RING) {
    settings_enter_requested = true;
    return;
  }

  if (active_count == 0) return;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);

  float dx = (float)p.x - EXAMPLE_LCD_H_RES / 2.0f;
  float dy = (float)p.y - EXAMPLE_LCD_V_RES / 2.0f;
  float dist = sqrtf(dx * dx + dy * dy);

  // The touch->fire path had no logging at all, which made a tap that never fired
  // indistinguishable from a tap that never arrived. Gated by DRAUPNIR_TRACE_INPUT.
  TRACE("[tap] CLICKED x=%d y=%d dist=%.1f inner=%d outer=%d active_count=%d\n",
                (int)p.x, (int)p.y, dist, RING_INNER_R, RING_OUTER_R, active_count);

  if (dist < RING_INNER_R) {
    TRACE("[tap] -> center, fire pos=%d\n", active_positions[selected_idx]);
    macros_request_fire(active_positions[selected_idx]);
  } else if (dist <= RING_OUTER_R + 10) {
    int idx = wedge_index_from_point(p.x, p.y);
    TRACE("[tap] -> wedge v=%d fire pos=%d\n", idx, active_positions[idx]);
    select_idx(idx);
    macros_request_fire(active_positions[idx]);
  } else {
    TRACE("[tap] -> outside ring, ignored\n");
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
  // LVGL delivers GESTURE or CLICKED for a touch, never both, so a gesture misfiring on
  // ordinary taps would silently swallow every macro fire. Gated by DRAUPNIR_TRACE_INPUT.
  TRACE("[tap] GESTURE dir=%d (bottom=%d) any_running=%d\n",
                (int)dir, (int)LV_DIR_BOTTOM, (int)macros_any_running());

  // Swipe UP: open Settings. Refused while the pairing overlay owns the screen -- a passkey
  // being replaced by a menu mid-pairing is unrecoverable without restarting the pairing.
  if (dir == LV_DIR_TOP) {
    if (ui_mode == UI_RING && !ble_pairing_active()) settings_open_requested = true;
    return;
  }

  if (dir != LV_DIR_BOTTOM) return;

  // Swipe DOWN inside Settings closes it. Safe to overload the panic gesture here ONLY because
  // opening Settings stopped every running macro, so nothing can be running behind the menu
  // (spec section 5). Do not reuse it as "back" anywhere a macro could still be live.
  if (ui_mode != UI_RING) {
    settings_close_requested = true;
    return;
  }

  // Swipe DOWN on the ring: unchanged kill-all.
  if (!macros_any_running()) return;
  Serial.println("[diag] swipe down -> kill all macros");
  macros_request_stop_all();
  haptics_pulse();
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
      // One final repaint on the running->idle edge, or the last-drawn pulse frame stays burned
      // on screen and the ring looks permanently "running".
      //
      // Lock FIRST, commit was_running AFTER -- the same ordering H5 established for
      // update_pairing_overlay(). Committing first meant a single 50ms lock timeout consumed the
      // transition permanently: the guard above would then return early forever and the stale
      // pulse frame would never be cleared. Leaving was_running set makes the next tick retry.
      if (!lvgl_lock(50)) return;
      lv_obj_invalidate(lv_scr_act());
      lvgl_unlock();
      was_running = false;
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
// pairing_overlay itself is declared at the top of this file (see the note there).
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
  if (!lvgl_lock(200)) return; // not clearing the flag on failure means the next tick retries
  Serial.println("[diag] profiles changed via BLE, reloading + rebuilding ring UI");
  // The reload itself must happen HERE, inside the lock -- not in ble_engine's save handler.
  // deserializeJson() clears and reallocates profilesDoc's pool, and ring_draw_event_cb reads
  // that same document from the LVGL task on every repaint. Reloading without the lock held is
  // a use-after-free on the renderer: the same defect class H3 fixed for the macro engine, on a
  // second reader that H3 did not cover.
  profiles_reload();
  rebuild_ring_layout();
  selected_idx = 0;
  if (active_count > 0) select_idx(selected_idx);
  lvgl_unlock();
  ble_clear_profiles_dirty();
}

static void settings_layout(void) {
  for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
    bool active = (i == settings_sel);
    lv_obj_set_style_text_font(settings_rows[i], active ? &orbitron_24 : &orbitron_14, 0);
    // LVGL's stock Montserrat has no bold face and no synthetic bold; Orbitron gives size a real
    // partner in opacity. Size + contrast carry the hierarchy (spec section 5).
    lv_obj_set_style_text_opa(settings_rows[i], active ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_align(settings_rows[i], LV_ALIGN_CENTER, 0, (i - settings_sel) * SETTINGS_ROW_PITCH);
  }
}

static void build_settings_overlay(void) {
  lv_obj_t *scr = lv_scr_act();
  settings_overlay = lv_obj_create(scr);
  lv_obj_set_size(settings_overlay, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
  lv_obj_set_pos(settings_overlay, 0, 0);
  lv_obj_set_style_bg_color(settings_overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(settings_overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(settings_overlay, 0, 0);
  lv_obj_set_style_border_width(settings_overlay, 0, 0);
  lv_obj_clear_flag(settings_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN);

  settings_list_panel = lv_obj_create(settings_overlay);
  lv_obj_set_size(settings_list_panel, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
  lv_obj_center(settings_list_panel);
  lv_obj_set_style_bg_opa(settings_list_panel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(settings_list_panel, 0, 0);
  lv_obj_clear_flag(settings_list_panel, LV_OBJ_FLAG_SCROLLABLE);

  // The two rails framing the centre slot -- the slot-machine affordance. Drawn first so the
  // rows, created after, paint on top in LVGL's insertion-order z-stacking.
  for (int i = 0; i < 2; i++) {
    lv_obj_t *rail = lv_obj_create(settings_list_panel);
    lv_obj_set_size(rail, 170, 2);
    lv_obj_set_style_bg_color(rail, lv_color_hex(0x404050), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_radius(rail, 0, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rail, LV_ALIGN_CENTER, 0, (i == 0 ? -1 : 1) * (SETTINGS_ROW_PITCH / 2 + 8));
  }

  for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
    lv_obj_t *row = lv_label_create(settings_list_panel);
    lv_label_set_text(row, SETTINGS_ITEMS[i].name);
    lv_obj_set_style_text_color(row, lv_color_white(), 0);
    lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
    settings_rows[i] = row;
  }
  settings_layout();

  settings_edit_panel = lv_obj_create(settings_overlay);
  lv_obj_set_size(settings_edit_panel, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
  lv_obj_center(settings_edit_panel);
  lv_obj_set_style_bg_opa(settings_edit_panel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(settings_edit_panel, 0, 0);
  lv_obj_clear_flag(settings_edit_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(settings_edit_panel, LV_OBJ_FLAG_HIDDEN);

  // Half-moon gauge across the TOP. LVGL arc angles put 0 deg at 3 o'clock and increase
  // clockwise -- the same convention wedge_center_angle() uses -- so 180..360 is 9 o'clock
  // through 12 to 3 o'clock, and the indicator fills left to right as the value rises. That
  // matches a clockwise knob turn, and reuses the ring's own geometry so the gauge reads as the
  // same object the rest of the UI is built from.
  gauge_arc = lv_arc_create(settings_edit_panel);
  lv_obj_set_size(gauge_arc, 300, 300);
  lv_obj_align(gauge_arc, LV_ALIGN_CENTER, 0, 0);
  lv_arc_set_rotation(gauge_arc, 0);
  lv_arc_set_bg_angles(gauge_arc, 180, 360);
  lv_arc_set_range(gauge_arc, 0, 100);
  lv_arc_set_value(gauge_arc, 0);
  lv_obj_remove_style(gauge_arc, NULL, LV_PART_KNOB);      // encoder drives it, not a drag handle
  lv_obj_clear_flag(gauge_arc, LV_OBJ_FLAG_CLICKABLE);     // taps must reach the screen click cb
  lv_obj_set_style_arc_width(gauge_arc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_width(gauge_arc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(gauge_arc, lv_color_hex(0x303040), LV_PART_MAIN);
  lv_obj_set_style_arc_color(gauge_arc, lv_color_white(), LV_PART_INDICATOR);

  gauge_value = lv_label_create(settings_edit_panel);
  lv_obj_set_style_text_font(gauge_value, &orbitron_24, 0);
  lv_obj_set_style_text_color(gauge_value, lv_color_white(), 0);
  lv_label_set_text(gauge_value, "0%");
  lv_obj_align(gauge_value, LV_ALIGN_CENTER, 0, 0);

  gauge_label = lv_label_create(settings_edit_panel);
  lv_obj_set_style_text_font(gauge_label, &orbitron_14, 0);
  lv_obj_set_style_text_color(gauge_label, lv_color_white(), 0);
  lv_obj_set_style_text_opa(gauge_label, LV_OPA_60, 0);
  lv_label_set_text(gauge_label, "Brightness");
  lv_obj_align(gauge_label, LV_ALIGN_CENTER, 0, 52);
}

// 0..100 across the usable duty range, so the floor reads as 0% rather than 8%.
static int duty_to_pct(int duty) {
  return ((duty - BRIGHTNESS_MIN) * 100) / (BRIGHTNESS_MAX - BRIGHTNESS_MIN);
}

static void gauge_refresh(void) {
  const setting_item_t *it = &SETTINGS_ITEMS[settings_sel];
  int pct = duty_to_pct(it->get());
  lv_arc_set_value(gauge_arc, pct);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  lv_label_set_text(gauge_value, buf);
  lv_label_set_text(gauge_label, it->name);
}

// One NVS write per Settings session, from the loop task. state_set_brightness() skips an
// identical write, so a session that changed nothing costs a read and no wear.
static void settings_commit(void) {
  if (!brightness_dirty) return;
  state_set_brightness(current_duty);
  brightness_dirty = false;
}

// Owns every Settings mode transition. Callbacks only raise flags (spec section 9).
static void update_settings(void) {
  if (settings_open_requested) {
    // Lock FIRST, clear the flag after -- the H5 lesson. Clearing first meant one lock timeout
    // consumed the transition permanently and the overlay never appeared again.
    if (!lvgl_lock(200)) return;
    settings_open_requested = false;

    // Killing here rather than via macros_request_stop_all() from the gesture callback keeps
    // "Settings is open" and "nothing is running" a single transition under one lock
    // acquisition, instead of two that can land in either order -- and avoids killing macros in
    // the case where the lock timed out and the overlay never opened. This runs on the loop
    // task, so calling macros_stop_all() directly is allowed here and ONLY here.
    macros_stop_all();

    ui_mode = UI_SETTINGS_LIST;
    settings_sel = 0;
    settings_last_activity = millis();
    settings_layout();
    lv_obj_add_flag(settings_edit_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(settings_list_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(settings_overlay);
    if (pairing_overlay) lv_obj_move_foreground(pairing_overlay);
    lvgl_unlock();
    Serial.println("[diag] settings opened, macros stopped");
    return;
  }

  if (ui_mode == UI_RING) return;

  if (settings_enter_requested) {
    if (!lvgl_lock(200)) return;   // flag stays set; next tick retries
    settings_enter_requested = false;
    if (ui_mode == UI_SETTINGS_LIST) {
      ui_mode = UI_SETTINGS_EDIT;
      gauge_refresh();
      lv_obj_add_flag(settings_list_panel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(settings_edit_panel, LV_OBJ_FLAG_HIDDEN);
      Serial.printf("[diag] settings: editing %s\n", SETTINGS_ITEMS[settings_sel].name);
    } else {
      ui_mode = UI_SETTINGS_LIST;
      lv_obj_add_flag(settings_edit_panel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(settings_list_panel, LV_OBJ_FLAG_HIDDEN);
      settings_commit();
      Serial.println("[diag] settings: back to list");
    }
    settings_last_activity = millis();
    lvgl_unlock();
    return;
  }

  bool timed_out = (millis() - settings_last_activity > SETTINGS_IDLE_TIMEOUT_MS);
  if (settings_close_requested || timed_out) {
    if (!lvgl_lock(200)) return;
    settings_close_requested = false;
    settings_commit();
    ui_mode = UI_RING;
    lv_obj_add_flag(settings_edit_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(settings_list_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_scr_act());
    lvgl_unlock();
    Serial.printf("[diag] settings closed (%s)\n", timed_out ? "idle timeout" : "swipe down");
  }
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

  build_settings_overlay();
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
    if (delta == 0) continue;
    if (!lvgl_lock(100)) continue;

    if (ui_mode == UI_SETTINGS_LIST) {
      // Clamp, no wrap -- matching the profile list (spec section 5). With one item this is a
      // no-op and the encoder does nothing here; that is expected, not a dead encoder.
      int next = settings_sel + delta;
      if (next < 0) next = 0;
      if (next > SETTINGS_ITEM_COUNT - 1) next = SETTINGS_ITEM_COUNT - 1;
      if (next != settings_sel) {
        settings_sel = next;
        settings_layout();
      }
      settings_last_activity = millis();
    } else if (ui_mode == UI_SETTINGS_EDIT) {
      const setting_item_t *it = &SETTINGS_ITEMS[settings_sel];
      it->apply(it->get() + delta * it->step);   // live preview: the panel changes as you turn
      gauge_refresh();
      settings_last_activity = millis();
    } else if (ui_mode == UI_RING) {
      // Re-check active_count INSIDE the lock. The pre-lock world can change while this task
      // waits: a profile reload on the loop task can drop it to 0, making the modulo a
      // divide-by-zero, which traps and reboots the S3. Capturing a pre-lock copy would not
      // help -- a shrunk count still indexes out of range.
      if (active_count > 0) {
        select_idx((selected_idx + delta + active_count) % active_count);
      }
    }
    lvgl_unlock();
  }
}

void setup() {
  Serial.begin(115200);
  // Refuse host-commanded reboots into the ROM bootloader. THIS IS WHAT WAS CAUSING THE
  // "unexplained resets into download mode" and the serial_capture.ps1 "The port is closed"
  // failures -- one bug, misfiled as two.
  //
  // USBCDC::_onLineState() (core USBCDC.cpp) runs a 4-state DTR/RTS machine and calls
  // usb_persist_restart(RESTART_BOOTLOADER) on the final step:
  //     !dtr&&rts  ->  dtr&&rts  ->  dtr&&!rts  ->  !dtr&&!rts  ->  BOOTLOADER
  // Opening a .NET SerialPort walks the first three (RTS asserts transiently, then DTR applies,
  // then RTS settles to the requested value); CLOSING it supplies the fourth. So any ordinary
  // open/close of a capture session reset the board -- on close, which is why the capture that
  // triggered it always looked clean and the NEXT one failed to find the port.
  //
  // Also disables the 1200-baud-touch reset (same guard, _onLineCoding).
  //
  // Nothing is lost: this board's auto-reset never worked anyway, so entering download mode is
  // already a physical BOOT-hold + replug (docs/Toolchain_arduino-cli.md). Serial output is
  // unaffected -- USBCDC::write() gates on tud_cdc_n_connected() (DTR), not on this machine.
  Serial.enableReboot(false);
  delay(2000);
  Serial.println("[diag] setup start");
  Serial.println("[diag] host-commanded bootloader reset DISABLED (enableReboot(false))");
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
  // DISABLED -- KNOWN ISSUE. haptics_init() breaks the CST816 touch controller: with it enabled,
  // touch events stop reaching LVGL entirely (no CLICKED and no GESTURE), while the encoder keeps
  // working. Confirmed on hardware by single-variable bisect -- disabling only this call restored
  // touch completely.
  //
  // The mechanism is I2C interference: this is the only code sharing bus 0 with the CST816 at
  // 0x15, and it probes all 112 addresses at boot, before lcd_lvgl_Init() registers the touch
  // input device.
  //
  // NOT yet narrowed to the specific cause -- either the blind bus scan or the DRV2605 register
  // access. Do NOT simply uncomment this: it will break touch again.
  //
  // Re-enable via single-variable tests, in this order (external audit 2026-08 proposed the same
  // two changes; run them separately or a pass tells you nothing about which one mattered):
  //   1. Drop the blind i2c_scan() from haptics_init(), leave the call site here. Touch back?
  //      -> the scan was the culprit; delete the scan permanently.
  //   2. Still broken? Restore the scan, instead move haptics_init() to AFTER lcd_lvgl_Init()
  //      so the CST816 input device is registered before anything else touches bus 0.
  //   3. Still broken? It is the DRV2605 register access itself -- next suspects are a bus speed
  //      or pull-up conflict, or the DRV2605 holding SDA. Scope the bus rather than guessing.
  // Re-test after each step by tapping a wedge AND swiping down; the encoder is not a valid
  // check, it kept working throughout the original failure.
  // See docs/HANDOFF.md.
  // haptics_init();
  Serial.println("[diag] haptics_init SKIPPED (known issue: breaks CST816 touch -- see HANDOFF.md)");


  lcd_lvgl_Init();
  Serial.printf("[diag] lcd_lvgl_Init done heap=%u\n", ESP.getFreeHeap());
  // Brightness: NVS if it has ever been set on the device, else the profile document's
  // settings.brightness as a seed. Was hardcoded to LCD_PWM_MODE_255, which meant
  // settings.brightness existed in the schema and did nothing.
  //
  // Ordering is load-bearing: profiles_init() above has already run state_init() and loaded
  // profilesDoc, so both the NVS handle and the seed are available here.
  uint8_t boot_brightness = state_brightness(profiles_default_brightness());
  lcd_bl_pwm_bsp_init(boot_brightness);
  current_duty = boot_brightness;  // so the gauge opens at the real value, not BRIGHTNESS_MAX
  Serial.printf("[diag] backlight init done duty=%u\n", (unsigned)boot_brightness);

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
  update_settings();
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
