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

// Profile indicators live INSIDE the inner hole: the ring band is full of wedges, and outside
// RING_OUTER_R there are only 8 px on a 360 px panel. That puts them in the tap-to-fire zone,
// so their hot zones switch profiles rather than firing -- something that looks tappable inside
// the fire zone must not fire a macro. Drawn as the real LV_SYMBOL_LEFT/RIGHT glyphs (Montserrat
// 28, see ring_draw_event_cb) rather than hand-drawn strokes -- see the note there.
#define INDICATOR_CX      72
// The hot zone is intentionally larger than the visual glyph -- a small hint with a generous
// tap target, not an oversight. Do not shrink these to match the glyph's own metrics.
#define HOTZONE_MIN_DX    60
#define HOTZONE_MAX_DY    40

// active_positions[v] = the profiles.json "pos" (0-15) shown at wedge v. selected_idx and all
// wedge/label indices below are in terms of v (0..active_count-1), not raw pos -- macros_fire()
// and profiles_find_macro() still take a real pos, so callers go through active_positions[].
static int active_positions[NUM_MACRO_SLOTS];
static int active_count = 0;

// ---- Settings menu ----------------------------------------------------------------------
// ui_mode is owned by loop(). LVGL-task callbacks (touch, gesture) and encoder_task only ever
// RAISE a request flag; loop() performs the transition under lvgl_lock(). Same pattern as
// update_pairing_overlay()/update_profiles_reload(). See spec section 9.
typedef enum { UI_RING, UI_SETTINGS_LIST, UI_SETTINGS_EDIT, UI_MODE_COUNT } ui_mode_t;
static ui_mode_t ui_mode = UI_RING;

// Each UI mode declares how it handles input, and the slot each handler occupies states which
// task runs it and under what lock. Before this table, three separate call sites each decided
// independently what a mode meant, and that produced two bugs of identical shape: screen_click_cb
// treating every non-ring mode as Settings, and three gesture sites each having to remember
// lv_indev_wait_release() -- the one that forgot let the panic gesture fire a macro into the host.
//
//   enter/exit    -- loop() task, lvgl_lock HELD by the caller. May call the macro engine.
//   on_encoder    -- encoder_task, lvgl_lock HELD by the caller. May touch LVGL.
//                    MUST NOT call the macro engine directly; use the request forms.
//   on_tap        -- LVGL task (lock already held by lv_timer_handler). Flags and LVGL only.
//   on_gesture    -- LVGL task, same rule. Returns true if it consumed the gesture.
//
// A NULL handler means "this mode does not care" and is treated exactly as returning false.
typedef struct {
  const char *name;                             // diagnostics only
  void (*enter)(void);
  void (*exit)(void);
  void (*on_encoder)(int delta);
  bool (*on_tap)(lv_coord_t x, lv_coord_t y);
  bool (*on_gesture)(lv_dir_t dir);
} ui_mode_def_t;

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

// Assigned (never accumulated) by the gesture callback and zeroed by loop(), so there is no
// cross-core read-modify-write. A very fast double-swipe may register as one switch; acceptable.
static volatile int8_t profile_switch_delta = 0;

static int selected_idx = 0;
static knob_handle_t s_knob = NULL;
static QueueHandle_t knob_queue = nullptr;   // int8_t deltas, one per knob event
static lv_obj_t *centre_macro   = nullptr;    // selected macro name
static lv_obj_t *centre_profile = nullptr;    // active profile name -- always visible now, so no
                                               // separate toast is needed to say what changed.

static uint32_t parse_hex_color(const char *hex, uint32_t fallback) {
  if (hex == nullptr || strlen(hex) < 6) return fallback;
  int offset = (hex[0] == '#') ? 1 : 0;
  return (uint32_t)strtol(hex + offset, nullptr, 16) & 0xFFFFFF;
}

// lv_draw_label does NOT clip to the lv_area_t passed to it -- that area only sets the wrap
// width (lv_draw_label.c:109) and the alignment offset (:174); a name wider than max_w wraps to
// a second line that paints ~14px below the box, over the ring band and the neighbouring wedge,
// in all nine outline/fill copies. Truncate with an ellipsis instead of relying on clipping that
// doesn't exist.
//
// Writes the (possibly truncated) name into out_buf and returns out_buf, so callers can pass the
// result straight to lv_draw_label without a second copy.
static const char *wedge_label_fit(const char *name, char *out_buf, size_t out_buf_sz, lv_coord_t max_w) {
  if (name == nullptr) name = "?";
  size_t len = strlen(name);
  if (len >= out_buf_sz) len = out_buf_sz - 1;
  memcpy(out_buf, name, len);
  out_buf[len] = '\0';

  if (lv_txt_get_width(out_buf, (uint32_t)strlen(out_buf), &orbitron_12, 0, LV_TEXT_FLAG_NONE) <= max_w) {
    return out_buf;
  }

  // Too wide: drop characters from the end until "<what's left>..." fits, or there is nothing
  // left to drop. Reserve room for the ellipsis up front so the loop below never has to re-check
  // buffer space, only pixel width.
  size_t ellipsis_room = (out_buf_sz >= 4) ? 3 : 0;
  size_t max_keep = (out_buf_sz > ellipsis_room) ? (out_buf_sz - 1 - ellipsis_room) : 0;
  if (len > max_keep) {
    len = max_keep;
    out_buf[len] = '\0';
  }

  // Budget for the ellipsis BEFORE choosing the prefix. Measuring only the kept prefix and
  // appending "..." afterwards overshoots by the ellipsis width -- 9 px in orbitron_12 -- which
  // is enough to wrap, and lv_draw_label paints the wrapped line outside the box.
  lv_coord_t ell_w = lv_txt_get_width("...", 3, &orbitron_12, 0, LV_TEXT_FLAG_NONE);
  lv_coord_t fit_w = (max_w > ell_w) ? (lv_coord_t)(max_w - ell_w) : (lv_coord_t)0;

  while (len > 1 &&
         lv_txt_get_width(out_buf, (uint32_t)len, &orbitron_12, 0, LV_TEXT_FLAG_NONE) > fit_w) {
    len--;
    out_buf[len] = '\0';
  }
  if (ellipsis_room > 0) {
    memcpy(out_buf + len, "...", 3);
    out_buf[len + 3] = '\0';
  }
  return out_buf;
}

// Mix a colour toward white by `f` (0..1). Used to light the selected wedge in its own hue
// rather than washing it out with a neutral highlight -- macro colours are user-chosen, so any
// fixed highlight colour is invisible on someone's palette.
static uint32_t brighten(uint32_t c, float f) {
  uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  r = (uint8_t)(r + (255.0f - r) * f);
  g = (uint8_t)(g + (255.0f - g) * f);
  b = (uint8_t)(b + (255.0f - b) * f);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
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

// Ring rotation in degrees, animated. wedge_center_angle() reads ring_rot; the selection sets
// ring_rot_target and loop() eases toward it. A whole-wedge snap (90 deg at 4 macros) is
// impossible to follow -- the owner read it as random rather than fast.
static float ring_rot = 0.0f;         // current, what is drawn
static float ring_rot_target = 0.0f;  // continuous: NOT wrapped, so easing takes the short way

// Center angle (in lv_draw_arc's own angle units) of wedge v, given `count` total wedges.
// Verified directly against lv_arc.c's own knob-placement code (the one part of LVGL
// guaranteed to use its angle units correctly): knob_x = R*sin(angle+90) = R*cos(angle),
// knob_y = R*sin(angle) -- i.e. plain x=R*cos(angle), y=R*sin(angle), which puts 0deg at
// 3 o'clock and makes increasing angle go CLOCKWISE on screen. The +270 offset rotates that
// so v=0 lands at 12 o'clock; since both v and the angle unit increase clockwise, no direction
// flip is needed between them.
// v*wedge_deg - ring_rot puts the wedge at ring_rot's own origin at the +270 point -- 12
// o'clock -- and rotates every other wedge around it. The ring moves under a fixed selection
// point rather than a highlight travelling around a fixed ring. When ring_rot equals
// selected_idx*wedge_deg (the settled state), wedge selected_idx sits at 12 o'clock, matching
// the pre-animation behaviour; mid-animation, ring_rot is whatever loop() has eased it to.
static float wedge_center_angle(int v, int count) {
  float wedge_deg = 360.0f / count;
  float a = fmodf(v * wedge_deg - ring_rot + 270.0f, 360.0f);
  if (a < 0.0f) a += 360.0f;
  return a;
}

// Rescans the active macro set and refreshes the centre stack. No wedge objects exist any more
// (task 8 deleted the per-wedge labels -- wedges are plain colour arcs drawn in
// ring_draw_event_cb), so there is nothing left here to re-create or re-parent. Kept as its own
// function, under its original name, because several callers (profile switch, BLE profile
// reload, initial build) depend on calling it as one step.
static void rebuild_ring_layout(void) {
  scan_active_positions();
  update_centre_stack();
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

    // Macro name on the wedge, white with a black outline so it is legible on every wedge colour
    // without the text colour changing between wedges. This is a SECONDARY cue -- the centre
    // stack carries the authoritative name at 21:1 on black.
    if (!macro.isNull()) {
      float rad = center * (float)M_PI / 180.0f;
      lv_coord_t lx = (lv_coord_t)(EXAMPLE_LCD_H_RES / 2.0f + RING_MID_R * cosf(rad));
      lv_coord_t ly = (lv_coord_t)(EXAMPLE_LCD_V_RES / 2.0f + RING_MID_R * sinf(rad));
      static const int8_t OUTLINE_OFS[8][2] = {
        {-1,-1}, {0,-1}, {1,-1}, {-1,0}, {1,0}, {-1,1}, {0,1}, {1,1},
      };
      // lv_draw_label does NOT clip to `coords` -- coords only sets the wrap width, so an
      // over-long name wraps to a second line that paints outside the box, nine times over,
      // across the neighbouring wedge. Truncate to fit instead (see wedge_label_fit()).
      char nmbuf[24];
      const char *nm = wedge_label_fit((const char *)(macro["name"] | "?"), nmbuf, sizeof(nmbuf), 84);
      lv_draw_label_dsc_t wl;
      lv_draw_label_dsc_init(&wl);
      wl.font  = &orbitron_12;
      wl.opa   = LV_OPA_COVER;
      wl.align = LV_TEXT_ALIGN_CENTER;

      // Eight black copies first, then white on top. LVGL has no text stroke; this is what an
      // outline costs. It is what makes the label legible on EVERY wedge colour instead of
      // only on the dark ones.
      wl.color = lv_color_black();
      for (int o = 0; o < 8; o++) {
        lv_area_t oa = { (lv_coord_t)(lx - 42 + OUTLINE_OFS[o][0]),
                         (lv_coord_t)(ly -  8 + OUTLINE_OFS[o][1]),
                         (lv_coord_t)(lx + 42 + OUTLINE_OFS[o][0]),
                         (lv_coord_t)(ly +  8 + OUTLINE_OFS[o][1]) };
        lv_draw_label(draw_ctx, &wl, &oa, nm, NULL);
      }
      wl.color = lv_color_white();
      lv_area_t wa = { (lv_coord_t)(lx - 42), (lv_coord_t)(ly - 8),
                       (lv_coord_t)(lx + 42), (lv_coord_t)(ly + 8) };
      lv_draw_label(draw_ctx, &wl, &wa, nm, NULL);
    }

    if (v == selected_idx) {
      // Selection = the slice lighting up (owner mockup), not a border around it. LVGL 8 has no
      // blur, so the glow is stacked arcs at falling opacity. lv_draw_arc's `radius` is the
      // OUTER edge and `width` extends INWARD, so wider passes bleed toward the centre and can
      // never overflow the panel.
      uint32_t lit = brighten(color, 0.55f);
      static const struct { int16_t extra; lv_opa_t opa; } SEL_GLOW[] = {
        { 22, 28 }, { 12, 48 },
      };
      for (unsigned g = 0; g < sizeof(SEL_GLOW) / sizeof(SEL_GLOW[0]); g++) {
        lv_draw_arc_dsc_t gl;
        lv_draw_arc_dsc_init(&gl);
        gl.color = lv_color_hex(lit);
        gl.opa   = SEL_GLOW[g].opa;
        gl.width = (RING_OUTER_R - RING_INNER_R) + SEL_GLOW[g].extra;
        lv_draw_arc(draw_ctx, &gl, &center_pt, RING_OUTER_R,
                    (uint16_t)lroundf(start), (uint16_t)lroundf(end));
      }
      // The wash itself -- mockup uses 24% opacity, which is 61/255.
      lv_draw_arc_dsc_t wash;
      lv_draw_arc_dsc_init(&wash);
      wash.color = lv_color_hex(lit);
      wash.opa   = 61;
      wash.width = RING_OUTER_R - RING_INNER_R;
      lv_draw_arc(draw_ctx, &wash, &center_pt, RING_OUTER_R,
                  (uint16_t)lroundf(start), (uint16_t)lroundf(end));
    }

    // Drawn last so it wins over the selection wash (a brightened tint in the wedge's own hue,
    // not white -- selection stopped being a flat white highlight in the mockup rework) when a
    // wedge is both selected and running -- "running" is the more urgent state, and a macro
    // looping unnoticed is exactly the failure this indicator exists to prevent.
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

  // Tinted bloom -- fakes a glow with stacked arcs, since LVGL 8 has no blur. Colour follows the
  // SELECTED macro so the centre text and the top wedge read as one object.
  if (active_count > 0) {
    JsonObject selm = profiles_find_macro(active_positions[selected_idx]);
    uint32_t glow = selm.isNull() ? 0xFFFFFF
                                  : parse_hex_color(selm["color"] | "#FFFFFF", 0xFFFFFF);
    // A crisp 2px ring at the wedge band's inner edge, with a soft halo bleeding both ways.
    // Mockup construction. `radius` is the arc's OUTER edge and `width` extends inward, so each
    // entry's span is (radius - width) .. radius.
    static const struct { int16_t radius_ofs; int16_t w; lv_opa_t opa; } HALO[] = {
      { 10, 24, 22 },   // spans 78..102 -- wide, faint, straddles the ring
      {  6, 14, 45 },   // spans 84..98  -- tighter, brighter
      {  1,  2, 255 },  // spans 91..93  -- the crisp ring itself
    };
    for (unsigned i = 0; i < sizeof(HALO) / sizeof(HALO[0]); i++) {
      lv_draw_arc_dsc_t b;
      lv_draw_arc_dsc_init(&b);
      b.color = lv_color_hex(glow);
      b.opa   = HALO[i].opa;
      b.width = HALO[i].w;
      lv_draw_arc(draw_ctx, &b, &center_pt, RING_INNER_R + HALO[i].radius_ofs, 0, 360);
    }
  }

  // Drawn in the active profile's own colour, and only when a profile exists in that direction.
  // screen_click_cb() gates its hot zones on the same condition, so at either end of the list a
  // tap there falls through and fires instead of switching.
  int pcount = profiles_count();
  int pidx   = profiles_active_index();
  if (pcount > 1) {
    // A real FontAwesome chevron glyph, not hand-drawn strokes -- three iterations of tuning
    // lv_draw_line stroke width/length/caps failed to read as a ❯; a typeface glyph has shaping
    // two straight lines do not. LV_SYMBOL_LEFT/RIGHT are already compiled into
    // lv_font_montserrat_28 (LV_FONT_MONTSERRAT_28 enabled in lv_conf.h for task 8).
    const lv_coord_t cx = EXAMPLE_LCD_H_RES / 2;
    const lv_coord_t cy = EXAMPLE_LCD_V_RES / 2;

    lv_draw_label_dsc_t sym;
    lv_draw_label_dsc_init(&sym);
    sym.font  = &lv_font_montserrat_28;
    sym.color = lv_color_hex(parse_hex_color(profiles_active_color(), 0xFFFFFF));
    sym.opa   = LV_OPA_COVER;
    sym.align = LV_TEXT_ALIGN_CENTER;

    if (pidx > 0) {
      lv_area_t a = { (lv_coord_t)(cx - INDICATOR_CX - 20), (lv_coord_t)(cy - 20),
                      (lv_coord_t)(cx - INDICATOR_CX + 20), (lv_coord_t)(cy + 20) };
      lv_draw_label(draw_ctx, &sym, &a, LV_SYMBOL_LEFT, NULL);
    }
    if (pidx < pcount - 1) {
      lv_area_t a = { (lv_coord_t)(cx + INDICATOR_CX - 20), (lv_coord_t)(cy - 20),
                      (lv_coord_t)(cx + INDICATOR_CX + 20), (lv_coord_t)(cy + 20) };
      lv_draw_label(draw_ctx, &sym, &a, LV_SYMBOL_RIGHT, NULL);
    }
  }
}

// Call under lvgl_lock(). Both lines change together -- the macro name follows the selection,
// the profile name follows a profile switch, and a switch changes both.
static void update_centre_stack(void) {
  if (active_count == 0) {
    lv_label_set_text(centre_macro, "(no macros)");
  } else {
    JsonObject m = profiles_find_macro(active_positions[selected_idx]);
    lv_label_set_text(centre_macro, m.isNull() ? "?" : (const char *)(m["name"] | "?"));
  }
  lv_label_set_text(centre_profile, profiles_active_name());
}

static void select_idx(int idx) {
  selected_idx = idx;
  // Shortest angular path. ring_rot_target is deliberately NOT wrapped to 0..360: wrapping it
  // would make a 3->0 selection step animate 270 degrees the long way round.
  float wedge_deg = 360.0f / (active_count > 0 ? active_count : 1);
  float desired = selected_idx * wedge_deg;
  float d = fmodf(desired - fmodf(ring_rot_target, 360.0f) + 540.0f, 360.0f) - 180.0f;
  ring_rot_target += d;
  update_centre_stack();
  lv_obj_invalidate(lv_scr_act());
}

// Inverse of wedge_center_angle() -- maps a tap point to a wedge index using the same angle
// convention (atan2f already returns angle in exactly lv_draw_arc's units, since that
// convention IS the standard x=R*cos/y=R*sin parametrization; see wedge_center_angle's comment).
// MUST stay in lockstep with wedge_center_angle()'s use of ring_rot -- both read the same
// animated offset, so a tap hits the wedge that is visually under the finger even mid-animation,
// not wherever it will end up once the ease settles.
static int wedge_index_from_point(lv_coord_t px, lv_coord_t py) {
  if (active_count == 0) return 0;
  float dx = (float)px - EXAMPLE_LCD_H_RES / 2.0f;
  float dy = (float)py - EXAMPLE_LCD_V_RES / 2.0f;
  float angle = atan2f(dy, dx) * 180.0f / (float)M_PI;
  if (angle < 0.0f) angle += 360.0f;
  float wedge_deg = 360.0f / active_count;
  int rel = (int)lroundf((angle - 270.0f + ring_rot) / wedge_deg);
  return ((rel % active_count) + active_count) % active_count;
}

// Forward declarations: settings_list_on_encoder()/settings_edit_on_encoder() below call these,
// but their bodies (settings_layout(), gauge_refresh()) are defined later in the file alongside
// the rest of the Settings panel. Declaring here rather than moving those definitions keeps this
// diff to the dispatch refactor only.
static void settings_layout(void);
static void gauge_refresh(void);

// A tap inside the inner radius (the center label area) fires whatever the encoder currently
// has selected, without changing the selection. A tap on the ring itself fires that wedge
// without changing the selection either -- the knob is the only thing that moves the ring.
// Taps outside the outer radius (round-glass bezel) are ignored.
static bool ring_on_tap(lv_coord_t px, lv_coord_t py) {
  if (active_count == 0) return true;
  float dx = (float)px - EXAMPLE_LCD_H_RES / 2.0f;
  float dy = (float)py - EXAMPLE_LCD_V_RES / 2.0f;
  float dist = sqrtf(dx * dx + dy * dy);

  // The touch->fire path had no logging at all, which made a tap that never fired
  // indistinguishable from a tap that never arrived. Gated by DRAUPNIR_TRACE_INPUT.
  TRACE("[tap] CLICKED x=%d y=%d dist=%.1f inner=%d outer=%d active_count=%d\n",
                (int)px, (int)py, dist, RING_INNER_R, RING_OUTER_R, active_count);

  if (dist < RING_INNER_R) {
    // The indicator hot zones. Live ONLY while their indicator is showing, so at either end of
    // the profile list the tap falls straight through and fires as it always has. The middle
    // ~120 px stays a comfortable fire target.
    int pcount = profiles_count();
    int pidx   = profiles_active_index();
    if (fabsf(dy) < HOTZONE_MAX_DY && fabsf(dx) > HOTZONE_MIN_DX) {
      if (dx < 0 && pidx > 0)              {
        TRACE("[tap] -> indicator prev\n");
        profile_switch_delta = -1; return true;
      }
      if (dx > 0 && pidx < pcount - 1)     {
        TRACE("[tap] -> indicator next\n");
        profile_switch_delta =  1; return true;
      }
    }
    TRACE("[tap] -> center, fire pos=%d\n", active_positions[selected_idx]);
    macros_request_fire(active_positions[selected_idx]);
    return true;
  }
  if (dist <= RING_OUTER_R + 10) {
    int idx = wedge_index_from_point(px, py);
    // Fire WITHOUT selecting. Since Task 8 the ring rotates, so select_idx() would spin the
    // tapped wedge up to 12 o'clock -- the owner reported that as unexpected. The knob owns
    // selection; a tap is a shortcut to fire, nothing more.
    TRACE("[tap] -> wedge v=%d fire pos=%d (no reselect)\n", idx, active_positions[idx]);
    macros_request_fire(active_positions[idx]);
    return true;
  }
  TRACE("[tap] -> outside ring, ignored\n");
  return true;   // outside the ring: consumed and ignored
}

// Returns false for LV_DIR_BOTTOM on purpose, so the dispatcher's default kill-all runs. Keeping
// kill-all in exactly one place is what stops a future mode from trapping a running macro.
static bool ring_on_gesture(lv_dir_t dir) {
  // Swipe UP: open Settings. Refused while the pairing overlay owns the screen -- a passkey
  // being replaced by a menu mid-pairing is unrecoverable without restarting the pairing.
  if (dir == LV_DIR_TOP) {
    if (!ble_pairing_active()) settings_open_requested = true;
    return true;
  }
  // Horizontal swipes switch profiles, matching M5_M6_config.ino:1375-1380 so the same gesture
  // means the same thing on both boards: swipe left = next, swipe right = previous.
  // Ignored while Settings or the pairing overlay owns the screen.
  if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
    if (!ble_pairing_active()) profile_switch_delta = (dir == LV_DIR_LEFT) ? 1 : -1;
    return true;
  }
  return false;
}

static void ring_on_encoder(int delta) {
  // Re-check INSIDE the lock: a profile reload on the loop task can drop active_count to 0 while
  // this task waits, making the modulo a divide-by-zero that traps and reboots the S3.
  if (active_count > 0) {
    // MINUS delta: turning the knob clockwise spins the RING clockwise, bringing the wedge
    // counter-clockwise of the top up to the selector. The dial is attached to the knob.
    select_idx((selected_idx - delta + active_count) % active_count);
  }
}

static bool settings_on_tap(lv_coord_t x, lv_coord_t y) {
  (void)x; (void)y;
  settings_enter_requested = true;
  return true;
}

// Swipe down closes; up and sideways are consumed and ignored (spec section 6's exits table).
// Returning true for BOTTOM deliberately suppresses the dispatcher's kill-all default: opening
// Settings already stopped every running macro, so there is nothing to kill.
static bool settings_on_gesture(lv_dir_t dir) {
  if (dir == LV_DIR_BOTTOM) settings_close_requested = true;
  return true;
}

static void settings_list_on_encoder(int delta) {
  // Clamp, no wrap -- matching the profile list (spec section 5). With one item this is a
  // no-op and the encoder does nothing here; that is expected, not a dead encoder.
  int next = settings_sel + delta;
  if (next < 0) next = 0;
  if (next > SETTINGS_ITEM_COUNT - 1) next = SETTINGS_ITEM_COUNT - 1;
  if (next != settings_sel) { settings_sel = next; settings_layout(); }
  settings_last_activity = millis();
}

static void settings_edit_on_encoder(int delta) {
  const setting_item_t *it = &SETTINGS_ITEMS[settings_sel];
  it->apply(it->get() + delta * it->step);   // live preview: the panel changes as you turn
  gauge_refresh();
  settings_last_activity = millis();
}

// Positional, in ui_mode_t order -- C++ does not portably support designated array initialisers,
// and the static_assert below is what catches a mismatch if the enum ever grows out of step.
static const ui_mode_def_t UI_MODES[UI_MODE_COUNT] = {
  /* UI_RING          */ { "ring",       NULL, NULL, ring_on_encoder,          ring_on_tap,     ring_on_gesture },
  /* UI_SETTINGS_LIST */ { "settings",   NULL, NULL, settings_list_on_encoder, settings_on_tap, settings_on_gesture },
  /* UI_SETTINGS_EDIT */ { "brightness", NULL, NULL, settings_edit_on_encoder, settings_on_tap, settings_on_gesture },
};
static_assert(sizeof(UI_MODES) / sizeof(UI_MODES[0]) == UI_MODE_COUNT,
              "UI_MODES must have exactly one entry per ui_mode_t value");

static inline const ui_mode_def_t *mode_def(void) { return &UI_MODES[ui_mode]; }

// loop() task only, with lvgl_lock ALREADY HELD by the caller.
static void ui_mode_set(ui_mode_t next) {
  if (next == ui_mode) return;
  const ui_mode_def_t *cur = mode_def();
  if (cur->exit) cur->exit();
  ui_mode = next;
  if (mode_def()->enter) mode_def()->enter();
}

// Dispatches to the active mode's on_tap handler; the per-mode tap semantics (what "inner
// radius", "ring band", etc. mean) live with each handler -- see ring_on_tap()/settings_on_tap().
static void screen_click_cb(lv_event_t *e) {
  (void)e;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  const ui_mode_def_t *m = mode_def();
  if (m->on_tap) m->on_tap(p.x, p.y);
  // No default. A tap no mode consumes does NOTHING -- firing a macro is ring-specific and
  // ring_on_tap does it. This makes "no non-ring mode may fire a macro" structural.
}

// Swipe down anywhere = stop every running macro. A gesture rather than an on-screen button
// because it can be performed without looking at the screen -- the same reasoning the M5Dial
// build used -- which is what you want when a runaway "toggle" macro is spamming the host and
// the screen is the last thing you're looking at.
//
// Runs on the LVGL task, so it must NOT call macros_stop_all() directly (see macro_engine.h);
// macros_request_stop_all() queues it for loop().
static void screen_gesture_cb(lv_event_t *e) {
  (void)e;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);

  // Unconditional, before dispatch: this touch is a gesture, so it is never also a tap, whatever
  // the mode decides below. LVGL sends LV_EVENT_CLICKED on release regardless of the gesture.
  //
  // Doing this per-branch has now failed three times: a swipe-up re-firing the macro Settings
  // had just stopped, a no-op swipe-down FIRING a macro when nothing was running, and a swipe
  // the handler explicitly refused activating a Settings item. There is no path where a swipe
  // should also count as a tap, so this belongs here rather than at each accepting branch.
  lv_indev_wait_release(indev);

  // LVGL delivers GESTURE or CLICKED for a touch, never both, so a gesture misfiring on
  // ordinary taps would silently swallow every macro fire. Gated by DRAUPNIR_TRACE_INPUT.
  TRACE("[tap] GESTURE dir=%d (bottom=%d) any_running=%d\n",
                (int)dir, (int)LV_DIR_BOTTOM, (int)macros_any_running());

  const ui_mode_def_t *m = mode_def();
  if (m->on_gesture && m->on_gesture(dir)) return;

  // Default-safe fall-through: an unhandled swipe-down still kills every running macro. A mode
  // that ignores gestures entirely therefore cannot trap a looping macro behind itself.
  if (dir == LV_DIR_BOTTOM && macros_any_running()) {
    Serial.println("[diag] swipe down -> kill all macros");
    macros_request_stop_all();
    haptics_pulse();
  }
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

// Eases ring_rot toward ring_rot_target. Runs on the loop task under lvgl_lock(), matching
// update_running_pulse() -- an lv_timer would run on the LVGL task and race the writes made
// from encoder_task.
static unsigned long last_ring_anim = 0;
static void update_ring_rotation(void) {
  if (ring_rot == ring_rot_target) return;
  unsigned long now = millis();
  if (now - last_ring_anim < 16) return;   // ~60 fps
  last_ring_anim = now;
  if (!lvgl_lock(50)) return;              // leave the delta pending; next tick retries
  float diff = ring_rot_target - ring_rot;
  if (fabsf(diff) < 0.5f) ring_rot = ring_rot_target;
  else                    ring_rot += diff * 0.58f;   // ease factor tuned via hardware feedback (3 rounds). Math: with 0.5° snap threshold, 90° step needs (1-f)^n*90 <= 0.5; n=6 frames gives f=0.58. At 16ms/frame: ~96ms settle time (vs ~140ms at 0.45f, ~230ms at 0.30f).
  lv_obj_invalidate(lv_scr_act());
  lvgl_unlock();
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
  // lv_obj_create() defaults every new object to LV_OBJ_FLAG_CLICKABLE (core/lv_obj.c:436), and
  // lv_obj_hit_test() requires that flag to report a hit (core/lv_obj_pos.c:950). A full-screen
  // container left clickable wins lv_indev_search_obj()'s hit test before it ever reaches `scr`,
  // silently swallowing every tap meant for screen_click_cb -- the only LV_EVENT_CLICKED handler
  // in this file. Clear it so taps fall through to the screen.
  lv_obj_clear_flag(settings_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN);

  settings_list_panel = lv_obj_create(settings_overlay);
  lv_obj_set_size(settings_list_panel, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
  lv_obj_center(settings_list_panel);
  lv_obj_set_style_bg_opa(settings_list_panel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(settings_list_panel, 0, 0);
  lv_obj_clear_flag(settings_list_panel, LV_OBJ_FLAG_SCROLLABLE);
  // Same lv_obj_create() default as settings_overlay above: clickable-by-default means this
  // full-screen panel steals the hit test from `scr` before screen_click_cb ever runs. Clear it.
  lv_obj_clear_flag(settings_list_panel, LV_OBJ_FLAG_CLICKABLE);

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
  // Same lv_obj_create() default as settings_overlay/settings_list_panel above: clickable-by-
  // default means this full-screen panel steals the hit test from `scr` before screen_click_cb
  // ever runs -- clearing LV_OBJ_FLAG_CLICKABLE on gauge_arc alone isn't enough, since this panel
  // sits underneath it and wins the hit test first. Clear it here too.
  lv_obj_clear_flag(settings_edit_panel, LV_OBJ_FLAG_CLICKABLE);
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

// 0..100 across the usable duty range, so the floor reads as 0% rather than 8%. Clamped for
// display only -- current_duty itself is never clamped here, so the boot value stays honest to
// whatever was actually stored (e.g. a pre-M7 profiles.json brightness below BRIGHTNESS_MIN,
// which would otherwise print as a negative percentage until the encoder is first turned).
static int duty_to_pct(int duty) {
  if (duty < BRIGHTNESS_MIN) return 0;
  if (duty > BRIGHTNESS_MAX) return 100;
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
//
// Split from the write itself: this only captures whether a write is needed and the value to
// write, so callers can do it while still holding lvgl_lock() and perform the actual flash
// write only after releasing it (see the call sites -- M4).
static bool settings_commit_prepare(uint8_t *out_duty) {
  if (!brightness_dirty) return false;
  *out_duty = current_duty;
  brightness_dirty = false;
  return true;
}

// Owns every Settings mode transition. Callbacks only raise flags (spec section 9).
static void update_settings(void) {
  if (settings_open_requested) {
    // Lock FIRST, clear the flag after -- the H5 lesson. Clearing first meant one lock timeout
    // consumed the transition permanently and the overlay never appeared again.
    if (!lvgl_lock(200)) return;
    settings_open_requested = false;
    // A CLICKED landing between loop()'s read of settings_enter_requested and this open branch
    // can leave that flag (or a stale close request) set from the session that just ended --
    // consumed on the NEXT open, jumping straight into the brightness editor instead of the
    // list. Clear both here so nothing leaks across a close/reopen.
    settings_enter_requested = false;
    settings_close_requested = false;

    // Killing here rather than via macros_request_stop_all() from the gesture callback keeps
    // "Settings is open" and "nothing is running" a single transition under one lock
    // acquisition, instead of two that can land in either order -- and avoids killing macros in
    // the case where the lock timed out and the overlay never opened. This runs on the loop
    // task, so calling macros_stop_all() directly is allowed here and ONLY here.
    macros_stop_all();

    ui_mode_set(UI_SETTINGS_LIST);
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
    bool commit_pending = false;
    uint8_t commit_duty = 0;
    if (ui_mode == UI_SETTINGS_LIST) {
      ui_mode_set(UI_SETTINGS_EDIT);
      gauge_refresh();
      lv_obj_add_flag(settings_list_panel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(settings_edit_panel, LV_OBJ_FLAG_HIDDEN);
      Serial.printf("[diag] settings: editing %s\n", SETTINGS_ITEMS[settings_sel].name);
    } else {
      ui_mode_set(UI_SETTINGS_LIST);
      lv_obj_add_flag(settings_edit_panel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(settings_list_panel, LV_OBJ_FLAG_HIDDEN);
      commit_pending = settings_commit_prepare(&commit_duty);
      Serial.println("[diag] settings: back to list");
    }
    settings_last_activity = millis();
    lvgl_unlock();
    // NVS write happens here, AFTER releasing lvgl_lock() -- the whole point of deferring it to
    // the loop task was that a flash write stalls; doing it while still holding the LVGL mutex
    // stalls the renderer exactly the same, just from inside the lock instead of outside it.
    if (commit_pending) state_set_brightness(commit_duty);
    return;
  }

  bool timed_out = (millis() - settings_last_activity > SETTINGS_IDLE_TIMEOUT_MS);
  if (settings_close_requested || timed_out) {
    if (!lvgl_lock(200)) return;
    settings_close_requested = false;
    uint8_t commit_duty = 0;
    bool commit_pending = settings_commit_prepare(&commit_duty);
    ui_mode_set(UI_RING);
    lv_obj_add_flag(settings_edit_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(settings_list_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_scr_act());
    lvgl_unlock();
    // Same reason as above: write after unlock, not while holding the LVGL mutex.
    if (commit_pending) state_set_brightness(commit_duty);
    Serial.printf("[diag] settings closed (%s)\n", timed_out ? "idle timeout" : "swipe down");
  }
}

// Runs the switch on the loop task: profiles_set_active() calls macros_stop_all() (loop-only)
// and rebuild_ring_layout() needs lvgl_lock(). Same hand-off update_profiles_reload() uses.
static void update_profile_switch(void) {
  int delta = profile_switch_delta;
  if (delta == 0) return;
  // Lock FIRST; only consume the request once the lock is held (the H5 lesson).
  if (!lvgl_lock(200)) return;
  profile_switch_delta = 0;

  if (profiles_set_active(profiles_active_index() + delta)) {
    rebuild_ring_layout();
    selected_idx = 0;
    if (active_count > 0) select_idx(selected_idx);
  }
  lvgl_unlock();
}

static void build_ring_ui(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_add_event_cb(scr, ring_draw_event_cb, LV_EVENT_DRAW_MAIN_END, NULL);
  lv_obj_add_event_cb(scr, screen_click_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

  // LVGL suppresses gestures entirely while a touch is scrolling something --
  // indev_gesture() returns immediately if proc->types.pointer.scroll_obj is set, before the
  // gesture thresholds are even consulted. lv_obj_create() makes every object scrollable by
  // default, including the screen, and the ring USED TO carry one absolutely-positioned wedge
  // label per macro, wide enough to overflow it horizontally (chord could reach ~177 px at 4
  // macros, putting the 3 o'clock label's right edge near x=400 on a 360 px panel). The screen
  // was therefore horizontally scrollable, and every horizontal swipe scrolled instead of
  // gesturing -- which is why profile switching could not be triggered at all while swipe-up,
  // with far less vertical overflow, worked only intermittently.
  //
  // Task 8 deleted the wedge labels (the ring is now plain colour arcs, see ring_draw_event_cb),
  // but the guard stays: the centre stack below is also absolutely positioned (lv_obj_align with
  // an offset, not lv_obj_center), and any future absolutely-positioned object that can extend
  // past the display bounds would silently reopen this exact bug. Found by hardware testing.
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  centre_macro = lv_label_create(scr);
  lv_obj_set_style_text_font(centre_macro, &orbitron_bold_24, 0);
  lv_obj_set_style_text_color(centre_macro, lv_color_white(), 0);
  lv_obj_set_width(centre_macro, 150);
  lv_obj_set_style_text_align(centre_macro, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(centre_macro, LV_LABEL_LONG_DOT);
  lv_obj_align(centre_macro, LV_ALIGN_CENTER, 0, -12);

  centre_profile = lv_label_create(scr);
  lv_obj_set_style_text_font(centre_profile, &orbitron_14, 0);
  lv_obj_set_style_text_color(centre_profile, lv_color_white(), 0);
  lv_obj_set_style_text_opa(centre_profile, LV_OPA_80, 0);
  lv_obj_set_width(centre_profile, 150);
  lv_obj_set_style_text_align(centre_profile, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(centre_profile, LV_LABEL_LONG_DOT);
  lv_obj_align(centre_profile, LV_ALIGN_CENTER, 0, 18);

  rebuild_ring_layout();
  if (active_count > 0) select_idx(selected_idx);

  build_settings_overlay();
  build_pairing_overlay();
}

// Callbacks run on the esp_timer task, not in an ISR -- plain xQueueSend (not the FromISR variant)
// is correct here.
static void _knob_left_cb(void *arg, void *data) {
  int8_t d = -1;
  if (knob_queue) xQueueSend(knob_queue, &d, 0);
}
static void _knob_right_cb(void *arg, void *data) {
  int8_t d = +1;
  if (knob_queue) xQueueSend(knob_queue, &d, 0);
}

static void encoder_task(void *arg) {
  for (;;) {
    int8_t d;
    if (xQueueReceive(knob_queue, &d, portMAX_DELAY) != pdTRUE) continue;
    int delta = d;
    // Coalesce anything already queued. Unlike the event group this replaced, nothing is lost:
    // every event contributes exactly once to the accumulated delta.
    while (xQueueReceive(knob_queue, &d, 0) == pdTRUE) delta += d;
    TRACE("[knob] delta=%d t=%lu\n", delta, (unsigned long)millis());
    if (delta == 0) continue;
    if (!lvgl_lock(100)) continue;
    const ui_mode_def_t *m = mode_def();
    if (m->on_encoder) m->on_encoder(delta);
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
  // Build stamp. Without this there is no way to tell from a serial capture which firmware is
  // actually on the board -- the flash pipeline is a BOOT-hold replug, an upload, and a second
  // physical replug, and a miss at any step is silent. One wasted debugging cycle was spent
  // asking whether a change had been flashed at all.
  Serial.printf("[diag] build %s %s\n", __DATE__, __TIME__);
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

  knob_queue = xQueueCreate(32, sizeof(int8_t));
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
#if DRAUPNIR_TRACE_INPUT
  // Raw electrical picture from the knob driver's ring buffer -- drained every tick (not just
  // when the [diag] heartbeat fires) so the buffer stays shallow and never has to overflow
  // under normal polling. Diagnostic only; the driver's decode/debounce path is untouched.
  {
    uint8_t st; uint32_t t;
    while (knob_debug_pop(&st, &t)) {
      Serial.printf("[quad] A=%u B=%u t=%lu\n",
                    (unsigned)((st >> 1) & 1), (unsigned)(st & 1), (unsigned long)t);
    }
    if (knob_debug_overflowed()) Serial.println("[quad] OVERFLOW -- samples dropped");
  }
#endif
  macros_update();
  ble_update();
  update_pairing_overlay();
  update_profiles_reload();
  update_settings();
  update_profile_switch();
  update_running_pulse();
  update_ring_rotation();
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
