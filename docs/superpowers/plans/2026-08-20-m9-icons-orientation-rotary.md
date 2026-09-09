# M9 — Orientation, icons, rotary mode, port parity — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring the Waveshare knob to behavioural parity with the M5Dial for every shared feature — dial orientation, macro icons, and rotary macro mode — on top of a mode-dispatch refactor that makes adding UI modes a table entry rather than three edits.

**Architecture:** A `ui_mode_def_t` dispatch table replaces three hand-written `ui_mode` switch sites, carrying per-handler threading contracts and default-safe fall-through. Orientation drives the panel's MADCTL register directly (hardware rotation, zero per-frame cost) plus a touch transform. Icons render the app-generated 18×18 XBM through LVGL's `ALPHA_1BIT` alpha path. Rotary mode becomes one table entry plus two fire-queue sentinels, because the macro engine is `loop()`-task only.

**Tech Stack:** Arduino ESP32 core 3.3.11, LVGL 8.4.0, ArduinoJson 7.4.3, LittleFS, `esp_lcd_sh8601`, CST816 touch, FreeRTOS.

**Spec:** `docs/superpowers/specs/2026-08-20-m9-icons-orientation-rotary-design.md`. Read it first — it carries the reasoning these tasks only implement, and the audit findings that shaped them.

---

## Global Constraints

- **FQBN (exact, do not guess):**
  `esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled`
- **Compile:** `./arduino-cli.exe compile --fqbn "<FQBN>" firmware/Waveshare_LVGL_Test` — run in the FOREGROUND with a 600000 ms timeout, capture the real exit code.
- **After every upload you must PHYSICALLY REPLUG the board.** esptool's closing reset is a no-op here; the board stays in download mode and serial goes silent, which looks exactly like a boot loop and is not.
- **Auto-reset does not work.** `No serial data received` means "hold BOOT and replug", not "broken board".
- **Serial capture:** `scripts/serial_capture.ps1 -Port <COM> -DurationSec <n> -LogPath <file>`. **Never `arduino-cli monitor`.**
- **Threading rules (these have caused shipped bugs):** LVGL objects only under `lvgl_lock()`; the macro engine is `loop()`-task only; input callbacks hand off via flags or the fire queue and must use the `macros_request_*` forms; acquire `lvgl_lock()` **before** clearing a pending request flag, or one lock timeout consumes the transition permanently.
- **Verify every LVGL and esp_lcd API against the installed headers** at `C:\Users\ido11\OneDrive\Documents\Arduino\libraries\lvgl\src\`. This project has shipped or nearly shipped **five** bugs from APIs that compile and silently do nothing.
- **Nothing absolutely positioned may exceed the 360×360 panel.** Label objects overflowing it once made the screen scrollable, and a scrolling object discards gestures before LVGL consults any threshold.
- **`DRAUPNIR_TRACE_INPUT` stays `0`** in every commit.
- **Schema `version` stays 2.** Bumping to 3 is M8b, out of scope.

### There is no host test framework — read this before Task 1

No `pytest`, no CI, nothing that can assert against a running board. Classic red-green TDD does not apply and fake test steps would be worse than none. Each task therefore:

1. **States its acceptance criterion first** — the exact observable behaviour that proves it works. Written before the implementation; this is what replaces the failing test.
2. **Implements.**
3. **Compile gate** — exit 0, mandatory, and the implementer's responsibility.
4. **Flash, replug, observe** — the human's. Implementers must not attempt it, simulate it, or claim it.
5. **Commit.**

Do not soften a criterion afterwards to match what happened.

---

## File Structure

| File | Responsibility |
|---|---|
| `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino` | Mode dispatch table and handlers, ring/settings/rotary UI, icon drawing, orientation application |
| `firmware/Waveshare_LVGL_Test/lcd_bsp.c` | MADCTL write helper, orientation-aware touch transform |
| `firmware/Waveshare_LVGL_Test/lcd_bsp.h` | Declarations for the above |
| `firmware/Waveshare_LVGL_Test/macro_engine.h/.cpp` | Rotary step sentinels and request API; rotary macro lifetime |
| `firmware/Waveshare_LVGL_Test/ble_engine.cpp` | `icon_xbm` strip on read, merge on save |
| `firmware/M5_M6_config/M5_M6_config.ino` | The same merge, closing the live icon-wipe bug |

Task order matters: **Task 1 is a prerequisite for Task 4** (rotary is a table entry). Tasks 2, 3 and 5 are independent of each other. Task 6 touches only the M5Dial.

---

## Task 1: Mode dispatch refactor

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino:46-47` (enum), `:466` (`screen_click_cb`), `:529` (`screen_gesture_cb`), `:849` (`update_settings`), `:1005` (`encoder_task`)

**Interfaces:**
- Produces: `ui_mode_def_t`, `UI_MODES[]`, `UI_MODE_COUNT`, `mode_def()`, `ui_mode_set(ui_mode_t)` — Task 4 adds one table entry and calls `ui_mode_set()`.

**This is a pure refactor. Behaviour must be identical afterwards.** That is what makes it independently verifiable: everything that worked before still works, and nothing new appears.

- [ ] **Step 1: Acceptance criterion**

After flashing, every existing interaction behaves exactly as it did: tap centre fires, tap wedge fires without re-selecting, tap a chevron switches profile, swipe up opens Settings and stops running macros, swipe down on the ring kills macros, swipe left/right switches profile, the encoder rotates the ring and scrolls the Settings list and drives the brightness gauge, swipe down closes Settings, and the 8 s idle timeout still closes it. **No new behaviour.** If anything changed, the refactor is wrong.

- [ ] **Step 2: Extend the enum and declare the table**

Replace line 46-47:

```c
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
```

- [ ] **Step 3: Extract the ring handlers**

Add these above the table, moving the bodies out of the existing callbacks verbatim. **Keep every
existing `TRACE(...)` call** in the moved code — they compile to nothing when `DRAUPNIR_TRACE_INPUT`
is 0, and they are the only visibility into the tap path when it is 1.

```c
static bool ring_on_tap(lv_coord_t px, lv_coord_t py) {
  if (active_count == 0) return true;
  float dx = (float)px - EXAMPLE_LCD_H_RES / 2.0f;
  float dy = (float)py - EXAMPLE_LCD_V_RES / 2.0f;
  float dist = sqrtf(dx * dx + dy * dy);

  if (dist < RING_INNER_R) {
    int pcount = profiles_count();
    int pidx   = profiles_active_index();
    if (fabsf(dy) < HOTZONE_MAX_DY && fabsf(dx) > HOTZONE_MIN_DX) {
      if (dx < 0 && pidx > 0)              { profile_switch_delta = -1; return true; }
      if (dx > 0 && pidx < pcount - 1)     { profile_switch_delta =  1; return true; }
    }
    macros_request_fire(active_positions[selected_idx]);
    return true;
  }
  if (dist <= RING_OUTER_R + 10) {
    int idx = wedge_index_from_point(px, py);
    macros_request_fire(active_positions[idx]);   // fire WITHOUT selecting -- the ring rotates
    return true;
  }
  return true;   // outside the ring: consumed and ignored
}

// Returns false for LV_DIR_BOTTOM on purpose, so the dispatcher's default kill-all runs. Keeping
// kill-all in exactly one place is what stops a future mode from trapping a running macro.
static bool ring_on_gesture(lv_dir_t dir) {
  if (dir == LV_DIR_TOP) {
    if (!ble_pairing_active()) settings_open_requested = true;
    return true;
  }
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
    select_idx((selected_idx - delta + active_count) % active_count);
  }
}
```

- [ ] **Step 4: Extract the settings handlers**

```c
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
  int next = settings_sel + delta;
  if (next < 0) next = 0;
  if (next > SETTINGS_ITEM_COUNT - 1) next = SETTINGS_ITEM_COUNT - 1;
  if (next != settings_sel) { settings_sel = next; settings_layout(); }
  settings_last_activity = millis();
}

static void settings_edit_on_encoder(int delta) {
  const setting_item_t *it = &SETTINGS_ITEMS[settings_sel];
  it->apply(it->get() + delta * it->step);
  gauge_refresh();
  settings_last_activity = millis();
}
```

- [ ] **Step 5: Build the table**

```c
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
```

- [ ] **Step 6: Rewrite the three dispatch sites**

`screen_click_cb` becomes:

```c
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
```

`screen_gesture_cb` becomes:

```c
static void screen_gesture_cb(lv_event_t *e) {
  (void)e;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);

  // Unconditional, before dispatch: this touch is a gesture, so it is never also a tap, whatever
  // the mode decides below. LVGL sends LV_EVENT_CLICKED on release regardless of the gesture.
  lv_indev_wait_release(indev);

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
```

`encoder_task`'s locked section becomes:

```c
    if (!lvgl_lock(100)) continue;
    const ui_mode_def_t *m = mode_def();
    if (m->on_encoder) m->on_encoder(delta);
    lvgl_unlock();
```

- [ ] **Step 7: Route transitions through `ui_mode_set()`**

In `update_settings()`, replace every direct assignment to `ui_mode` with `ui_mode_set(...)`. The surrounding lock acquisition, flag clearing order, panel show/hide and `settings_commit()` calls stay exactly as they are — only the assignment changes.

- [ ] **Step 8: Compile**

```bash
cd F:/Projects/Draupnir/draupnir
./arduino-cli.exe compile --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled" firmware/Waveshare_LVGL_Test
```

Expected: exit 0. A `static_assert` failure means the enum and table disagree — fix the table, not the assert.

- [ ] **Step 9: Commit**

```bash
git add firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino
git commit -m "refactor(ui): dispatch input through a mode table

Three call sites each decided independently what a UI mode meant, and that
produced two bugs of identical shape: screen_click_cb treating every
non-ring mode as Settings, and three gesture sites each having to remember
lv_indev_wait_release() -- the one that forgot let a no-op swipe-down fire
a macro into the attached host.

Each mode now declares its own handlers, and the slot a handler occupies
states which task runs it and under what lock. Two defaults make the
dangerous cases safe by construction rather than by everyone remembering:
an unhandled swipe-down still kills macros, and an unhandled tap does
nothing rather than firing one.

Pure refactor -- behaviour is unchanged."
```

---

## Task 2: Dial orientation

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/lcd_bsp.c` (init MADCTL, touch transform, new setter), `lcd_bsp.h` (declarations)
- Modify: `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino` (`setup()`, the profile-reload path)
- Modify: `firmware/Waveshare_LVGL_Test/macro_engine.h/.cpp` (orientation accessor)

**Interfaces:**
- Produces: `void lcd_set_orientation(uint8_t o)` in `lcd_bsp.h`; `uint8_t profiles_orientation(void)` in `macro_engine.h`.

- [ ] **Step 1: Acceptance criterion**

Changing Dial Orientation in the app and saving rotates the display **and** touch, live, without a replug, for **all four** values. Tapping a wedge after rotation fires the macro under the finger, not one elsewhere. **180° and 270° are the primary cases** — in use the cable must exit the top or right of the dial, so those two must work, and they are the two that are unproven.

- [ ] **Step 2: Add the orientation accessor**

`macro_engine.h`:

```c
// settings.orientation, 0..3 (0/90/180/270 degrees). 0 if absent or out of range.
// profiles.json is the SINGLE source of truth for this -- deliberately unlike brightness, which
// is NVS-authoritative. Brightness has one writer (the device); orientation will have two once
// the Settings menu gains an entry, and NVS cannot reconcile two writers without the device
// writing back to JSON anyway -- at which point NVS is a redundant second copy.
uint8_t profiles_orientation(void);
```

`macro_engine.cpp`, beside `profiles_default_brightness()`:

```c
uint8_t profiles_orientation(void) {
  int o = profilesDoc["settings"]["orientation"] | 0;
  if (o < 0 || o > 3) o = 0;
  return (uint8_t)o;
}
```

- [ ] **Step 3: Make MADCTL runtime-settable**

In `lcd_bsp.c`, replace the compile-time `#ifdef EXAMPLE_Rotate_90` MADCTL entry in the init command list with the 0° value `{0x36, (uint8_t[]){0x00}, 1, 0}`, and add:

```c
// Rotation is done by the PANEL via MADCTL (0x36), not in software. Do not reach for esp_lcd's
// rotation API or LVGL's sw_rotate: panel_sh8601_swap_xy() returns ESP_ERR_NOT_SUPPORTED
// (esp_lcd_sh8601.c:319) and mirror_y is unsupported too, while LVGL's software rotation would
// re-rotate every flush on a device already rendering ten stripes per frame.
//
// 0x00 and 0x60 are proven on this hardware. 0xC0 and 0xA0 are the conventional 180/270 values
// and are UNVERIFIED -- and they are the two the owner actually needs, because the cable must
// exit the top or right of the dial in use.
static const uint8_t MADCTL_FOR_ORIENTATION[4] = { 0x00, 0x60, 0xC0, 0xA0 };
static uint8_t s_orientation = 0;

void lcd_set_orientation(uint8_t o)
{
  if (o > 3) o = 0;
  s_orientation = o;
  uint8_t madctl = MADCTL_FOR_ORIENTATION[o];
  esp_lcd_panel_io_tx_param(io_handle, 0x36, &madctl, 1);
}
```

`io_handle` is the existing `esp_lcd_panel_io_handle_t` in this file — check its actual name before
use and match it. Declare `lcd_set_orientation` in `lcd_bsp.h` inside the existing `extern "C"`
block. The touch callback reads the file-static `s_orientation` directly, so no getter is needed.

- [ ] **Step 4: Transform touch to match**

Replace the `#ifdef EXAMPLE_Rotate_90` block in `example_lvgl_touch_cb()`:

```c
    // The touch panel is not rotated by MADCTL -- only the display is -- so its coordinates must
    // be mapped into the rotated display frame here. Only the 90 degree case is proven (it is the
    // vendor's own); 180 and 270 are derived and must be confirmed by eye. If a rotation displays
    // correctly but taps land wrong, THIS is the suspect, not the MADCTL value.
    switch (s_orientation) {
      case 1:  data->point.x = tp_y;                       data->point.y = EXAMPLE_LCD_V_RES - tp_x; break;
      case 2:  data->point.x = EXAMPLE_LCD_H_RES - tp_x;   data->point.y = EXAMPLE_LCD_V_RES - tp_y; break;
      case 3:  data->point.x = EXAMPLE_LCD_V_RES - tp_y;   data->point.y = tp_x;                     break;
      default: data->point.x = tp_x;                       data->point.y = tp_y;                     break;
    }
```

Keep the existing clamps to `EXAMPLE_LCD_H_RES` / `V_RES` that follow.

- [ ] **Step 5: Apply at boot and on reload**

In `setup()`, immediately after `lcd_lvgl_Init()`:

```c
  lcd_set_orientation(profiles_orientation());
```

In `update_profiles_reload()` — the existing path that runs under `lvgl_lock()` after a BLE save — add after `profiles_reload()`:

```c
  // Orientation is applied live so the screen rotates the moment the app saves. MADCTL and the
  // touch transform are both cheap; neither needs a reboot.
  lcd_set_orientation(profiles_orientation());
```

- [ ] **Step 6: Compile, then commit**

Compile (exit 0), then:

```bash
git add firmware/Waveshare_LVGL_Test/lcd_bsp.c firmware/Waveshare_LVGL_Test/lcd_bsp.h \
        firmware/Waveshare_LVGL_Test/macro_engine.h firmware/Waveshare_LVGL_Test/macro_engine.cpp \
        firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino
git commit -m "feat(m9): consume settings.orientation via MADCTL

The app has had a Dial Orientation dropdown all along and the Waveshare
ignored it entirely -- the M5Dial has consumed it since before this port.
A port gap, not a new feature.

Rotation is done by the panel via MADCTL, not in software: swap_xy is
ESP_ERR_NOT_SUPPORTED on this panel and LVGL's sw_rotate would re-rotate
every flush on a device already rendering ten stripes per frame.

profiles.json is the single source of truth, deliberately unlike
brightness -- orientation will have two writers once Settings gains an
entry, and NVS cannot reconcile two writers without the device writing
back to JSON anyway.

180 and 270 degrees are UNVERIFIED and are the two that matter most.

Observed on hardware: <FILL IN -- all four orientations, display and touch>"
```

---

## Task 3: Macro icons

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino` (`ring_draw_event_cb`, new helpers)

**Interfaces:**
- Consumes: `contrast_on(uint32_t)` (existing), `wedge_label_fit()` (existing).
- Produces: `static bool wedge_icon_decode(const char *hex, uint8_t *out54)`.

- [ ] **Step 1: Acceptance criterion**

A macro with an icon assigned in the app shows that icon on its wedge, **upright and unmirrored**, in the same black-or-white contrast colour the labels use. A macro without one still shows its truncated name. A profile with a mix shows a mix.

- [ ] **Step 2: Add the decoder**

```c
// The app renders each Feather icon to an 18x18 monochrome bitmap and stores it as `icon_xbm`:
// 108 hex characters = 54 bytes = 3 bytes per row. Returns false unless the string is exactly
// that, so a malformed or absent value falls back to the name rather than drawing garbage.
//
// THE BIT REVERSAL IS MANDATORY. XBM is LSB-first (bit 0 is the leftmost pixel); LVGL reads
// pos = 7 - (x & 0x7) (lv_img_decoder.c:599), which is MSB-first. Without reversing, every glyph
// renders mirrored within each byte -- it compiles and draws SOMETHING, which is exactly how this
// class of bug survives review.
static bool wedge_icon_decode(const char *hex, uint8_t *out54) {
  if (hex == nullptr || strlen(hex) != 108) return false;
  for (int b = 0; b < 54; b++) {
    char pair[3] = { hex[b * 2], hex[b * 2 + 1], '\0' };
    char *end = nullptr;
    long v = strtol(pair, &end, 16);
    if (end != pair + 2) return false;      // non-hex character
    uint8_t x = (uint8_t)v;
    x = (uint8_t)(((x & 0xF0) >> 4) | ((x & 0x0F) << 4));
    x = (uint8_t)(((x & 0xCC) >> 2) | ((x & 0x33) << 2));
    x = (uint8_t)(((x & 0xAA) >> 1) | ((x & 0x55) << 1));
    out54[b] = x;
  }
  return true;
}
```

- [ ] **Step 3: Draw the icon in place of the label**

In `ring_draw_event_cb`, replace the wedge-label block with:

```c
    uint8_t iconbits[54];
    const char *ixbm = macro.isNull() ? nullptr : (const char *)(macro["icon_xbm"] | (const char *)nullptr);
    if (wedge_icon_decode(ixbm, iconbits)) {
      // ALPHA_1BIT supplies alpha only; the colour comes from recolor/recolor_opa
      // (lv_draw_img.h:38). 18x18 is load-bearing: it is the interchange size shared with the
      // app and the M5Dial, AND LV_IMG_BUF_SIZE_ALPHA_1BIT ((w/8)+1)*h disagrees with the
      // decoder's stride (w+7)>>3 at any width that is a multiple of 8. Do not change it.
      lv_img_dsc_t idata;
      idata.header.cf         = LV_IMG_CF_ALPHA_1BIT;
      idata.header.always_zero = 0;
      idata.header.reserved   = 0;
      idata.header.w          = 18;
      idata.header.h          = 18;
      idata.data_size         = 54;
      idata.data              = iconbits;

      lv_draw_img_dsc_t idsc;
      lv_draw_img_dsc_init(&idsc);
      idsc.recolor     = contrast_on(color);
      idsc.recolor_opa = LV_OPA_COVER;

      lv_area_t ia = { (lv_coord_t)(lx - 9), (lv_coord_t)(ly - 9),
                       (lv_coord_t)(lx + 8), (lv_coord_t)(ly + 8) };
      lv_draw_img(draw_ctx, &idsc, &ia, &idata);
    } else {
      ...existing nine-draw outlined label block, unchanged...
    }
```

`lx`/`ly` are the existing wedge label centre. The area is 18 px wide (`-9`..`+8` inclusive) — LVGL areas are inclusive on both edges, so `+9` would be 19 px.

**Verify `lv_img_dsc_t`'s field names and `lv_draw_img`'s signature against the installed headers before writing this.** Report what you found.

- [ ] **Step 4: Compile, then commit**

```bash
git add firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino
git commit -m "feat(m9): render macro icons on the ring

The app has generated 18x18 XBM icon data into every macro all along and
the M5Dial has drawn it since before this port; the Waveshare ignored it.
Icons were never a greenfield asset pipeline -- only the rendering was
missing.

XBM is LSB-first and LVGL's ALPHA_1BIT is MSB-first, so every byte is
reversed. Without that it still draws something, mirrored per byte.

A wedge shows its icon if it has one and falls back to the truncated name
if not; the centre stack already carries the authoritative name at 21:1.

Observed on hardware: <FILL IN -- icons upright and unmirrored>"
```

---

## Task 4: Rotary macro mode

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/macro_engine.h/.cpp` (sentinels, request API, rotary macro lifetime, delete the false comment at `:448`)
- Modify: `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino` (new mode, overlay, table entry)

**Interfaces:**
- Consumes: `ui_mode_def_t`, `UI_MODES[]`, `ui_mode_set()` (Task 1).
- Produces: `void macros_request_rotary_step(int dir)`, `bool macros_rotary_active(void)`, `const char *macros_rotary_name(void)`, `uint32_t macros_rotary_color(void)`.

- [ ] **Step 1: Acceptance criterion**

Firing a macro whose `mode` is `"rotary"` shows a rotary screen instead of playing its actions. The encoder then drives `actions[0]` clockwise and `actions[1]` counter-clockwise, repeatably. Tapping returns to the ring. Swipe-down still kills all running macros from that screen.

- [ ] **Step 2: Add the rotary step plumbing to the macro engine**

`macro_engine.h`:

```c
// Rotary macros bind the encoder instead of playing. The encoder runs on its own task and the
// macro engine is loop()-only, so a turn must be ENQUEUED, exactly as macros_request_fire() does.
// dir > 0 executes actions[0]; dir < 0 executes actions[1].
void macros_request_rotary_step(int dir);

// True while a rotary macro owns the encoder. The UI reads these to draw its screen.
bool        macros_rotary_active(void);
const char *macros_rotary_name(void);
// Returns the raw "#RRGGBB" string, not a parsed value: parse_hex_color() lives in the .ino and
// the macro engine stays free of display concerns.
const char *macros_rotary_color(void);
```

`macro_engine.cpp`, beside `MACRO_CMD_STOP_ALL`:

```c
#define MACRO_CMD_ROTARY_CW  (-97)
#define MACRO_CMD_ROTARY_CCW (-98)

// Holds a JsonObject into profilesDoc, exactly like ActiveMacro -- so it MUST be dropped whenever
// the document is reloaded, or a save arriving while the rotary screen is up dereferences a freed
// pool. Same defect class H3 fixed, on a new trigger. macros_stop_all() clears it.
static JsonObject rotaryMacro;
static bool       rotaryActive = false;
```

In `macros_stop_all()`, alongside clearing `runningMacros[]`:

```c
  rotaryActive = false;
  rotaryMacro  = JsonObject();   // drop the reference into the old document pool
```

In `macros_fire()`, before the toggle handling:

```c
  const char *mode = macro["mode"] | "play_once";
  if (strcmp(mode, "rotary") == 0) {
    rotaryMacro  = macro;
    rotaryActive = true;
    return;               // a rotary macro binds the encoder; it does not play
  }
```

In `macros_update()`'s drain loop, alongside the `MACRO_CMD_STOP_ALL` case:

```c
    } else if (pendingPos == MACRO_CMD_ROTARY_CW || pendingPos == MACRO_CMD_ROTARY_CCW) {
      if (rotaryActive && !rotaryMacro.isNull()) {
        JsonArray acts = rotaryMacro["actions"];
        int idx = (pendingPos == MACRO_CMD_ROTARY_CW) ? 0 : 1;
        if ((int)acts.size() > idx) executeAction(acts[idx]);
      }
```

And the accessors:

```c
void macros_request_rotary_step(int dir) {
  if (!fireQueue) return;
  int cmd = (dir > 0) ? MACRO_CMD_ROTARY_CW : MACRO_CMD_ROTARY_CCW;
  xQueueSend(fireQueue, &cmd, 0);
}
bool        macros_rotary_active(void) { return rotaryActive; }
const char *macros_rotary_name(void)   { return rotaryMacro.isNull() ? "Rotary"  : (const char *)(rotaryMacro["name"]  | "Rotary"); }
const char *macros_rotary_color(void)  { return rotaryMacro.isNull() ? "#FFFFFF" : (const char *)(rotaryMacro["color"] | "#FFFFFF"); }
```

The `.ino` parses the colour with its existing `parse_hex_color()`.

**Delete the comment at `macro_engine.cpp:448`** claiming rotary is handled by the caller. It never was.

- [ ] **Step 3: Add the rotary overlay and mode entry**

Add `UI_ROTARY` to `ui_mode_t` **before** `UI_MODE_COUNT`, build an overlay mirroring `build_settings_overlay()` (full-screen, black, `LV_OBJ_FLAG_SCROLLABLE` and `LV_OBJ_FLAG_CLICKABLE` both cleared — a full-screen container steals every tap otherwise), containing:

- the macro name centred, `&orbitron_bold_24`, in the macro's colour
- `TURN DIAL` above at `&orbitron_14`, dimmed
- `TAP TO EXIT` below at `&orbitron_14`, dimmed
- left and right `LV_SYMBOL_LEFT`/`LV_SYMBOL_RIGHT` chevrons in the macro's colour, using `&lv_font_montserrat_28` as the ring indicators do

Declare the exit flag beside the other request flags, matching their `volatile bool` convention —
it is written on the LVGL task and read by `loop()`:

```c
static volatile bool rotary_exit_requested = false;
```

Handlers and table entry:

```c
static bool rotary_on_tap(lv_coord_t x, lv_coord_t y) {
  (void)x; (void)y;
  rotary_exit_requested = true;
  return true;
}
static void rotary_on_encoder(int delta) {
  macros_request_rotary_step(delta);   // request form: encoder_task must not run actions itself
}
// on_gesture is NULL on purpose: swipe-down falls through to the dispatcher's kill-all default.
// Unlike Settings, entering rotary mode does NOT stop running macros, so the panic gesture must
// still work here.
/* UI_ROTARY */ { "rotary", NULL, NULL, rotary_on_encoder, rotary_on_tap, NULL },
```

In `loop()`, add an `update_rotary()` next to `update_settings()` that enters the mode when `macros_rotary_active()` becomes true, exits on `rotary_exit_requested` (calling `macros_stop_all()` to clear the rotary macro), and shows/hides the overlay — all under `lvgl_lock()`, acquiring the lock **before** clearing the flag.

- [ ] **Step 4: Compile, then commit**

```bash
git add firmware/Waveshare_LVGL_Test/macro_engine.h firmware/Waveshare_LVGL_Test/macro_engine.cpp \
        firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino
git commit -m "feat(m9): implement rotary macro mode

The app has offered Rotary Dial as a macro mode all along and validates
that such macros have two actions; the M5Dial implements it. The Waveshare
silently ran them as play_once, firing both actions in sequence instead of
binding them to the knob -- a gap that failed quietly in the wrong
direction. macro_engine.cpp even carried a comment inherited from the
M5Dial claiming rotary was handled by the caller. It never was; deleted.

Unlike the M5Dial, which calls executeAction() straight from its loop, the
encoder here runs on its own task and the macro engine is loop()-only, so
a turn enqueues a step through the existing fire queue.

The active rotary macro holds a JsonObject into profilesDoc, so
macros_stop_all() clears it -- a profile save arriving while the rotary
screen is up would otherwise be an H3-class use-after-free.

Observed on hardware: <FILL IN -- encoder drives both actions, tap exits>"
```

---

## Task 5: `icon_xbm` strip on read and merge on save

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/ble_engine.cpp` (`get_profiles`, `save_profiles`)

**These must land together.** Stripping without merging ships a data-loss bug — do not split this task.

- [ ] **Step 1: Acceptance criterion**

`get_profiles` responses no longer carry `icon_xbm` (visible as a smaller `streamed N bytes` in the serial log). Editing **one** macro in the app and saving leaves **every other macro's icon intact** on the device after the reload.

- [ ] **Step 2: Strip on read**

Add a filtering sink that wraps `BleChunkSink` and drops `,"icon_xbm":"<hex>"` byte by byte, matching the marker with a small state machine, then skipping to the closing quote. The app needs only the icon *name*; the bitmap is display-side data, ~122 bytes per macro.

Mirror the M5Dial's approach (`M5_M6_config.ino`, the `ICON_XBM_MARKER` state machine) rather than inventing a second one — but note the Waveshare's sink already streams with near-zero peak heap, so this is a bandwidth saving, not the stability fix it was there.

- [ ] **Step 3: Merge on save**

In the `save_profiles` handler, **before** the temp-file write:

```c
    // The app sends its WHOLE in-memory document on save, and only ever writes icon_xbm for the
    // macro being edited (editor_panel.dart:106). Combined with stripping on read, that means
    // editing one macro would wipe every other macro's icon. This is a live bug on the M5Dial
    // today. Merging here makes stripping safe against ANY client -- the app, a hand-edited
    // profiles.json, nRF Connect, a future desktop client -- rather than trusting one to behave.
    //
    // Runs on the in-memory document before serialization, so it composes with the H4 atomic
    // write (temp -> verify -> rename) rather than replacing it.
```

For each profile index and each macro in the incoming `profilesObj`, if the macro has no `icon_xbm` and the currently loaded `profilesDoc` has a macro with the same `pos` in the same profile index that does, copy the stored value across.

Look the stored macro up by `pos`, not by array position — the app may reorder.

- [ ] **Step 4: Compile, then commit**

```bash
git add firmware/Waveshare_LVGL_Test/ble_engine.cpp
git commit -m "feat(m9): strip icon_xbm on read, merge it on save

The app needs only the icon name; the 54-byte bitmaps are display-side
data costing ~122 bytes per macro on every fetch.

Stripping alone is UNSAFE and must never ship without the merge. The app
sends its whole in-memory document on save and only writes icon_xbm for
the macro being edited, so a stripped read plus a save wipes every other
macro's icon. That is live on the M5Dial today.

Merging on the device makes stripping safe against any client rather than
trusting one to behave.

Observed on hardware: <FILL IN -- edit one macro, others keep their icons>"
```

---

## Task 6: Port the merge fix to the M5Dial

**Files:**
- Modify: `firmware/M5_M6_config/M5_M6_config.ino` (`save_profiles` handler)

- [ ] **Step 1: Acceptance criterion**

On the M5Dial, editing one macro in the app and saving leaves every other macro's icon intact. This is a **live data-loss bug** on that board today.

- [ ] **Step 2: Apply the same merge**

The M5Dial already strips (`ICON_XBM_MARKER`) and does not merge. Add the same preserve-by-`pos` pass before it writes `profiles.json`, with a comment recording that stripping without merging destroys icons and that the fix mirrors the Waveshare's.

The implementations differ — different JSON plumbing, different display stack — but the observable behaviour must not. That is the parity requirement for M9.

- [ ] **Step 3: Compile check**

The M5Dial has its own FQBN:

```bash
./arduino-cli.exe compile --fqbn "m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB" firmware/M5_M6_config
```

If the M5Stack core is not installed, **say so and stop** rather than skipping the gate silently — report it as `NEEDS_CONTEXT`.

- [ ] **Step 4: Commit**

```bash
git add firmware/M5_M6_config/M5_M6_config.ino
git commit -m "fix(m5): merge preserved icon_xbm on save

The M5Dial strips icon_xbm from get_profiles but never merged it back on
save. Since the app sends its whole in-memory document and only writes
icon_xbm for the macro being edited, editing one macro silently destroyed
every other macro's icon on this board.

Found while auditing the M5Dial for port gaps during M9. The Waveshare
gets the same merge; the implementations differ but the behaviour must
not."
```

---

## Final verification pass

Run all of this on one build, after Task 6.

- [ ] All four orientations rotate display **and** touch, live on save. **180° and 270° especially** — they are the ones that matter and the ones that are unproven.
- [ ] After rotating, tapping a wedge fires the macro under the finger.
- [ ] A runtime orientation change leaves partial-refresh regions correct (the spec's open risk). If it corrupts, fall back to boot-only and say so.
- [ ] Icons render upright and unmirrored; un-iconned macros still show names.
- [ ] A rotary macro binds the knob; both directions work; tap exits.
- [ ] Swipe-down kills macros from the ring **and** from the rotary screen.
- [ ] Settings still opens on swipe-up and stops running macros; swipe-down closes it.
- [ ] Tap centre fires; tap wedge fires without re-selecting; chevron taps switch profiles.
- [ ] Editing one macro in the app preserves every other macro's icon — **on both boards**.
- [ ] Heap flat; no `rst:0x` anywhere in the capture.

Then update `docs/Draupnir_Spec.md` §10 and `docs/HANDOFF.md`, recording only what was observed.
