# M7 + M8 — Persistence, Settings menu, profile switching — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Waveshare knob remember its brightness and active profile across power cycles, adjustable on-device through a gesture-driven Settings menu and profile switching.

**Architecture:** A new `device_state` module takes sole ownership of NVS (`Preferences`), keeping it free of LVGL, `ledc`, and JSON so the M5Dial can adopt it unchanged. `macro_engine` keeps the JSON profile store and gains a profile-switch API. All new UI lives in the `.ino`: LVGL-task callbacks only raise request flags, and `loop()` owns every mode transition, NVS write, and `macros_stop_all()` call under `lvgl_lock()`.

**Tech Stack:** Arduino ESP32 core (Espressif), LVGL 8.4.0, ArduinoJson 7.4.3, `Preferences`/NVS, LittleFS, TinyUSB HID.

**Spec:** `docs/superpowers/specs/2026-08-07-m7-m8-persistence-design.md`. Read it before starting — it carries the reasoning behind decisions this plan only implements.

---

## Global Constraints

- **FQBN (exact, do not guess):**
  `esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled`
- **Compile:** `./arduino-cli.exe compile --fqbn "<FQBN>" firmware/Waveshare_LVGL_Test`
- **Upload:** `./arduino-cli.exe upload -p <PORT> --fqbn "<FQBN>" firmware/Waveshare_LVGL_Test`
- **After every upload you must PHYSICALLY REPLUG the board.** esptool's closing reset is a no-op here; the board stays in download mode and runs nothing. Serial returns complete silence, which looks exactly like a boot loop and is not.
- **Auto-reset does not work.** `No serial data received` means "hold BOOT and replug", not "the board is broken".
- **USB-C plug orientation selects which MCU you reach.** VID `0x303A` = the ESP32-S3 (correct). VID `0x1A86` / 4 MB flash = wrong way round, rotate the plug 180°. The owner marked the S3 side of the cable green.
- **Serial capture:** `powershell -ExecutionPolicy Bypass -File scripts/serial_capture.ps1 -Port <COM> -DurationSec <n> -LogPath <file>`. **Never `arduino-cli monitor`** — it treats non-interactive stdin as an immediate quit.
- **Threading rules (these have caused shipped bugs):** the macro engine is `loop()`-task only; UI callbacks must use `macros_request_fire()` / `macros_request_stop_all()`, never `macros_fire()` / `macros_stop_all()`; touch LVGL objects only under `lvgl_lock()`; BLE callbacks hand off via flags drained in `loop()`; stop running macros before any profile reload or switch.
- **Lock-failure rule (the H5 lesson):** acquire `lvgl_lock()` *before* clearing a pending request flag. Clearing first means a single lock timeout consumes the transition permanently. On lock failure, leave the flag set and return so the next tick retries.
- **Verify LVGL APIs against the installed headers** (`$(./arduino-cli.exe config get directories.user)/libraries/lvgl/src/`). This codebase has been bitten twice by APIs that compile and silently do nothing.
- **Never claim hardware verification you did not perform.** State plainly what was observed and what was not.
- **No Wi-Fi, no HTTP server, no captive portal, no web UI.** Permanently cut. No application-layer auth token.
- **Schema `version` stays 2.** Bumping to 3 is M8b, out of scope here.

### There is no host test framework — read this before Task 1

This is firmware. There is no `pytest`, no CI, and nothing that can assert on a running board. Classic red-green TDD does not apply, and a plan that pretended otherwise would produce fake test steps.

The cycle used throughout is the project's established one (CLAUDE.md: *"Incremental milestones, each verified on hardware before advancing"*):

1. **State the acceptance criterion first** — the exact serial line or on-screen behaviour that will prove the task works. This is written *before* the implementation, and it is what replaces the failing test.
2. **Implement.**
3. **Compile gate** — must exit 0 with no new warnings.
4. **Flash, replug, observe** — capture serial and check the criterion literally. If the observation does not match, the task is not done.
5. **Commit.**

Each task's Step 1 is its criterion. Do not skip it, and do not soften it afterwards to match what happened.

---

## File Structure

| File | Responsibility |
|---|---|
| `firmware/Waveshare_LVGL_Test/device_state.h` | **New.** NVS-backed device state API. No LVGL, no `ledc`, no JSON. |
| `firmware/Waveshare_LVGL_Test/device_state.cpp` | **New.** Sole owner of `Preferences`, namespace `draupnir`. |
| `firmware/Waveshare_LVGL_Test/orbitron_12.c` `orbitron_14.c` `orbitron_24.c` | **New, generated.** LVGL font C arrays. |
| `firmware/Waveshare_LVGL_Test/lv_conf.h` | Declare the custom fonts, move `LV_FONT_DEFAULT` off Montserrat. |
| `firmware/Waveshare_LVGL_Test/macro_engine.h/.cpp` | Drops `Preferences`. Gains profile count/index/switch and the brightness + colour accessors. |
| `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino` | All new UI: Settings overlay, brightness gauge, profile switching, indicators, toast. |
| `docs/LICENSES/` | **New.** SIL OFL 1.1 text for the bundled font. |

Task order matters: Task 1 (fonts) is a prerequisite for Tasks 3–4, which reference `&orbitron_24`. Task 5 (second default profile) is a prerequisite for Tasks 6–7, which cannot be exercised with a single profile.

---

## Task 1: Orbitron fonts

**Files:**
- Create: `firmware/Waveshare_LVGL_Test/orbitron_12.c`, `orbitron_14.c`, `orbitron_24.c`
- Create: `docs/LICENSES/OFL-1.1-Orbitron.txt`
- Modify: `firmware/Waveshare_LVGL_Test/lv_conf.h:396-402`
- Modify: `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino:127`
- Modify: `README.md` (licence note)

**Interfaces:**
- Produces: `extern const lv_font_t orbitron_12, orbitron_14, orbitron_24;` — Tasks 3 and 4 use `&orbitron_24` and `&orbitron_14` directly.

- [ ] **Step 1: Acceptance criterion**

Write this down before touching anything. After flashing, the ring's macro labels and the centre label render in Orbitron — a wide, geometric, squarish face, unmistakably different from Montserrat. No label shows a box, a blank, or garbage. Record how much earlier wedge labels truncate than before; that observation decides whether the spec §5a fallback is needed.

- [ ] **Step 2: Fetch the TTF**

```powershell
New-Item -ItemType Directory -Force tools/fonts
Invoke-WebRequest -Uri "https://github.com/google/fonts/raw/main/ofl/orbitron/Orbitron%5Bwght%5D.ttf" -OutFile tools/fonts/Orbitron.ttf
Invoke-WebRequest -Uri "https://github.com/google/fonts/raw/main/ofl/orbitron/OFL.txt" -OutFile docs/LICENSES/OFL-1.1-Orbitron.txt
```

`Orbitron[wght].ttf` is a **variable font** spanning weight 400–900. `lv_font_conv` uses opentype.js, which renders the default instance — weight 400 (Regular). That is the expected result. The M5Dial's `Orbitron_Light_24` is an older static Light cut that is no longer published; Waveshare text will read slightly heavier. This is a known, accepted difference, not a defect.

`tools/fonts/` holds a build input, not source. Add it to `.gitignore`; the generated `.c` files are what get committed.

- [ ] **Step 3: Generate the three sizes**

```powershell
npx lv_font_conv --font tools/fonts/Orbitron.ttf --size 12 --bpp 4 --format lvgl `
  --range 0x20-0x7F --no-compress --lv-include lvgl.h `
  -o firmware/Waveshare_LVGL_Test/orbitron_12.c
npx lv_font_conv --font tools/fonts/Orbitron.ttf --size 14 --bpp 4 --format lvgl `
  --range 0x20-0x7F --no-compress --lv-include lvgl.h `
  -o firmware/Waveshare_LVGL_Test/orbitron_14.c
npx lv_font_conv --font tools/fonts/Orbitron.ttf --size 24 --bpp 4 --format lvgl `
  --range 0x20-0x7F --no-compress --lv-include lvgl.h `
  -o firmware/Waveshare_LVGL_Test/orbitron_24.c
```

- `--range 0x20-0x7F` — ASCII only. Macro names outside it already do not render today.
- `--no-compress` — avoids needing `LV_USE_FONT_COMPRESSED`, which is off in `lv_conf.h`.
- `--lv-include lvgl.h` — the sketch's include path resolves `lvgl.h`; the tool's default `lvgl/lvgl.h` does not.
- The C symbol is derived from the output filename, so `orbitron_12.c` defines `orbitron_12`.

Sizes map one-for-one onto what the UI uses today (12 wedge labels, 14 default, 24 new) so the face is the only variable that changes. Do not take the opportunity to resize anything.

- [ ] **Step 4: Declare the fonts in `lv_conf.h`**

Replace lines 396-402:

```c
/*Optionally declare custom fonts here.
 *You can use these fonts as default font too and they will be available globally.
 *E.g. #define LV_FONT_CUSTOM_DECLARE   LV_FONT_DECLARE(my_font_1) LV_FONT_DECLARE(my_font_2)*/
#define LV_FONT_CUSTOM_DECLARE   LV_FONT_DECLARE(orbitron_12) LV_FONT_DECLARE(orbitron_14) LV_FONT_DECLARE(orbitron_24)

/*Always set a default font*/
#define LV_FONT_DEFAULT &orbitron_14
```

Leave `LV_FONT_MONTSERRAT_12` and `_14` set to `1`. They cost a few KB and are the documented fallback if Orbitron proves unreadable in the wedges (spec §5a) — removing them now would mean regenerating to test the fallback.

- [ ] **Step 5: Point the wedge labels at Orbitron**

`Waveshare_LVGL_Test.ino:127`, inside `rebuild_ring_layout()`:

```c
    lv_obj_set_style_text_font(label, &orbitron_12, 0);
```

The centre label and the pairing label carry no explicit font, so they pick up `LV_FONT_DEFAULT` and need no change.

- [ ] **Step 6: Record the licence**

Orbitron is **SIL OFL 1.1**, not MIT. The OFL permits bundling only with its notice retained. Add to `README.md` under the licence section:

```markdown
### Bundled font

The firmware embeds Orbitron (SIL Open Font License 1.1), generated into LVGL font tables
as `firmware/Waveshare_LVGL_Test/orbitron_*.c`. The licence text is in
`docs/LICENSES/OFL-1.1-Orbitron.txt`. The rest of the project remains MIT.
```

- [ ] **Step 7: Compile**

```bash
./arduino-cli.exe compile --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled" firmware/Waveshare_LVGL_Test
```

Expected: exit 0. If it fails with `lvgl.h: No such file`, the `--lv-include` flag was wrong — regenerate rather than hand-editing three generated files.

- [ ] **Step 8: Flash, replug, observe**

Upload, **physically replug**, then look at the screen. Check the Step 1 criterion literally. Note the truncation observation in the commit message — it is the input to a real decision, not a nicety.

- [ ] **Step 9: Commit**

```bash
git add firmware/Waveshare_LVGL_Test/orbitron_*.c firmware/Waveshare_LVGL_Test/lv_conf.h \
        firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino docs/LICENSES/ README.md .gitignore
git commit -m "feat(ui): Orbitron replaces Montserrat on the ring

Matches the M5Dial firmware and the companion app, which both already use
Orbitron; the Waveshare build was the only surface that did not.

fonts::Orbitron_Light_24 is an M5GFX built-in the LVGL build cannot reach,
so the face is generated into LVGL font tables with lv_font_conv at the
three sizes the UI already uses (12/14/24) -- no element changes size, so
the face is the only variable.

Generated from the variable font's default instance (weight 400); the
M5Dial's static Light cut is no longer published, so Waveshare text reads
slightly heavier. Known and accepted.

Orbitron is OFL 1.1, not MIT: licence text and notice added.

Wedge label truncation observed on hardware: <FILL IN>"
```

---

## Task 2: `device_state` module and brightness at boot

**Files:**
- Create: `firmware/Waveshare_LVGL_Test/device_state.h`, `device_state.cpp`
- Modify: `firmware/Waveshare_LVGL_Test/macro_engine.cpp:4,15,183,196` and `macro_engine.h`
- Modify: `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino:503`

**Interfaces:**
- Produces: `state_init()`, `state_active_profile(int)`, `state_set_active_profile(int)`, `state_brightness(uint8_t)`, `state_set_brightness(uint8_t)` — Task 4 calls `state_set_brightness`, Task 6 calls `state_set_active_profile`.
- Produces: `profiles_default_brightness()` — used by the `.ino` in this task only.

- [ ] **Step 1: Acceptance criterion**

Serial at boot shows `[state] nvs opened (namespace=draupnir)` and `[diag] backlight init done duty=160`, and **the screen is visibly dimmer than before** (160/255 instead of full duty). If the duty line says 255, the seed is not being read.

- [ ] **Step 2: Create `device_state.h`**

```c
#pragma once
#include <stdint.h>

// Small, device-owned state that survives a power cycle, in NVS (Preferences, namespace
// "draupnir"). Spec section 8 calls this "State: last profile + brightness in NVS".
//
// Deliberately free of LVGL, ledc and JSON. Callers apply the values; this file only stores
// them. That is what lets the M5Dial adopt this file unchanged and swap in
// M5Dial.Display.setBrightness() (spec section 3).
//
// profiles.json is SEED ONLY. Callers pass the JSON value in as `fallback`; once a value has
// been written here it wins forever. NEVER copy a JSON value back over a written one: the
// companion app has no brightness control, so it round-trips a stale settings.brightness on
// every macro edit and would stomp whatever was set on the knob. The same argument applies to
// activeProfile once profiles_set_active() exists -- an app save carrying a stale index would
// silently revert an on-device profile switch.
void state_init();

int  state_active_profile(int fallback);
void state_set_active_profile(int idx);

uint8_t state_brightness(uint8_t fallback);
void    state_set_brightness(uint8_t duty);
```

- [ ] **Step 3: Create `device_state.cpp`**

```cpp
#include "device_state.h"
#include <Arduino.h>
#include <Preferences.h>

static Preferences prefs;

// Namespace kept as "draupnir" and the key as "activeProfile" so an already-deployed board
// keeps its stored profile across this refactor. Both keys are within NVS's 15-character limit
// ("activeProfile" is 13).
static const char *NVS_NAMESPACE  = "draupnir";
static const char *KEY_ACTIVE     = "activeProfile";
static const char *KEY_BRIGHTNESS = "brightness";

void state_init() {
  prefs.begin(NVS_NAMESPACE, false);
  Serial.println("[state] nvs opened (namespace=draupnir)");
}

int state_active_profile(int fallback) {
  return prefs.getInt(KEY_ACTIVE, fallback);
}

// Read-before-write on both setters: NVS is flash, and these are called from gesture handlers.
// Skipping an identical write costs one read and avoids a wear cycle on every no-op.
void state_set_active_profile(int idx) {
  if (prefs.getInt(KEY_ACTIVE, -1) == idx) return;
  prefs.putInt(KEY_ACTIVE, idx);
  Serial.printf("[state] activeProfile -> %d (persisted)\n", idx);
}

uint8_t state_brightness(uint8_t fallback) {
  return prefs.getUChar(KEY_BRIGHTNESS, fallback);
}

void state_set_brightness(uint8_t duty) {
  if (prefs.getUChar(KEY_BRIGHTNESS, 0) == duty) return;
  prefs.putUChar(KEY_BRIGHTNESS, duty);
  Serial.printf("[state] brightness -> %u (persisted)\n", (unsigned)duty);
}
```

- [ ] **Step 4: Move NVS ownership out of `macro_engine.cpp`**

Line 4 — replace `#include <Preferences.h>` with:

```cpp
#include "device_state.h"
```

Line 15 — delete `static Preferences prefs;` entirely.

Line 183, in `profiles_reload()`:

```cpp
  activeProfileIdx = state_active_profile(0);
```

Line 196-197, in `profiles_init()` — replace the `prefs.begin(...)` pair with:

```cpp
  state_init();
```

- [ ] **Step 5: Add the brightness seed accessor**

`macro_engine.h`, after `profiles_active_name()`:

```c
// The active document's settings.brightness -- the SEED for NVS brightness, never a source of
// truth once NVS has been written (see device_state.h). 160 if absent or out of range.
uint8_t profiles_default_brightness();
```

`macro_engine.cpp`, after `profiles_active_name()`:

```cpp
uint8_t profiles_default_brightness() {
  int b = profilesDoc["settings"]["brightness"] | 160;
  if (b < 0)   b = 0;
  if (b > 255) b = 255;
  return (uint8_t)b;
}
```

- [ ] **Step 6: Apply brightness at boot**

`Waveshare_LVGL_Test.ino` — add `#include "device_state.h"` next to the other project includes at the top.

Replace line 503-504:

```cpp
  // Brightness: NVS if it has ever been set on the device, else the profile document's
  // settings.brightness as a seed. Was hardcoded to LCD_PWM_MODE_255, which meant
  // settings.brightness existed in the schema and did nothing.
  //
  // Ordering is load-bearing: profiles_init() above has already run state_init() and loaded
  // profilesDoc, so both the NVS handle and the seed are available here.
  uint8_t boot_brightness = state_brightness(profiles_default_brightness());
  lcd_bl_pwm_bsp_init(boot_brightness);
  Serial.printf("[diag] backlight init done duty=%u\n", (unsigned)boot_brightness);
```

- [ ] **Step 7: Compile**

Run the compile command from Global Constraints. Expected: exit 0. A `'prefs' was not declared` error means Step 4 missed a use — grep `macro_engine.cpp` for `prefs.` and convert each.

- [ ] **Step 8: Flash, replug, capture**

```powershell
powershell -ExecutionPolicy Bypass -File scripts/serial_capture.ps1 -Port <COM> -DurationSec 20 -LogPath scratch/task2.log
```

Check the Step 1 criterion. The screen being visibly dimmer is as much a part of it as the log line — a correct log line with an unchanged screen means `lcd_bl_pwm_bsp_init` is not honouring its argument, which is a different bug.

- [ ] **Step 9: Commit**

```bash
git add firmware/Waveshare_LVGL_Test/device_state.* firmware/Waveshare_LVGL_Test/macro_engine.* \
        firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino
git commit -m "feat(m7): device_state owns NVS; brightness applied at boot

settings.brightness has been in the schema since the first profile store
and has never done anything -- the backlight was hardcoded to full duty.
It is now the seed for an NVS-backed value.

NVS ownership moves out of macro_engine, which held Preferences only
because it happened to need activeProfile. device_state is free of LVGL,
ledc and JSON so the M5Dial can adopt it unchanged (spec section 3).

Namespace and key names unchanged, so a deployed board keeps its stored
profile across the refactor. Both setters read before writing to avoid a
flash wear cycle on a no-op write."
```

---

## Task 3: Settings overlay, slot-machine list, open/close gestures

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino` (declarations near line 34, `rebuild_ring_layout` line 143, `screen_click_cb` line 231, `screen_gesture_cb` line 267, `build_ring_ui` line 394, `encoder_task` line 418, `loop` line 532)

**Interfaces:**
- Consumes: `&orbitron_14`, `&orbitron_24` (Task 1).
- Produces: `ui_mode_t` / `ui_mode`, `SETTINGS_ITEMS[]`, `setting_item_t`, `settings_sel`, `settings_last_activity`, `settings_layout()`, `settings_enter_requested` — Task 4 extends all of these.
- Produces: `BRIGHTNESS_MIN` = 20, `BRIGHTNESS_MAX` = 255, `BRIGHTNESS_STEP` = 16, `current_duty`, `brightness_get()`, `brightness_apply(int)` — Task 4 builds the gauge on them.

- [ ] **Step 1: Acceptance criterion**

Four observations, all required:

1. Swipe up on the ring → the Settings overlay appears with `Brightness` centred and large.
2. Fire the `Caps Lock` toggle so it loops (`any_running=1` in the `[diag] loop alive` line), then swipe up → serial shows `[diag] settings opened, macros stopped` **and** the next `loop alive` line shows `any_running=0`. Both, not just the overlay.
3. Swipe down → back to the ring, serial `[diag] settings closed (swipe down)`.
4. Open it and wait → `[diag] settings closed (idle timeout)` after ~8 s.

With one item the list **cannot scroll** and the encoder does nothing in Settings. That is expected (spec §5) — do not chase it.

- [ ] **Step 2: Add the mode state and item table**

In `Waveshare_LVGL_Test.ino`, after the `pairing_overlay` declaration (line 34):

```cpp
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
```

`setUpdutySubdivide` comes from `lcd_bl_pwm_bsp.h`, already included at line 5.

- [ ] **Step 3: Build the overlay**

Add before `build_ring_ui()`:

```cpp
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

  // The two rails framing the centre slot -- the slot-machine affordance. Drawn first so the
  // rows, created after, paint on top in LVGL's insertion-order z-stacking.
  for (int i = 0; i < 2; i++) {
    lv_obj_t *rail = lv_obj_create(settings_overlay);
    lv_obj_set_size(rail, 170, 2);
    lv_obj_set_style_bg_color(rail, lv_color_hex(0x404050), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_radius(rail, 0, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rail, LV_ALIGN_CENTER, 0, (i == 0 ? -1 : 1) * (SETTINGS_ROW_PITCH / 2 + 8));
  }

  for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
    lv_obj_t *row = lv_label_create(settings_overlay);
    lv_label_set_text(row, SETTINGS_ITEMS[i].name);
    lv_obj_set_style_text_color(row, lv_color_white(), 0);
    lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
    settings_rows[i] = row;
  }
  settings_layout();
}
```

- [ ] **Step 4: Create it, and fix the overlay z-order**

In `build_ring_ui()` (line ~408), insert **before** `build_pairing_overlay()`:

```cpp
  build_settings_overlay();
  build_pairing_overlay();
```

Order matters: later-created children paint on top, so pairing must be created last and stay above Settings.

In `rebuild_ring_layout()`, replace the single-overlay re-assert at line 143:

```cpp
  // Re-assert overlay z-order after re-creating the wedge labels above, or one profile save is
  // enough to leave macro names bleeding through an overlay. Settings first, pairing last, so
  // pairing always wins.
  if (settings_overlay) lv_obj_move_foreground(settings_overlay);
  if (pairing_overlay)  lv_obj_move_foreground(pairing_overlay);
```

- [ ] **Step 5: Route the gestures**

Replace the body of `screen_gesture_cb()` after the `TRACE` call:

```cpp
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
```

- [ ] **Step 6: Make taps stop reaching the macro engine**

At the very top of `screen_click_cb()`, before the `active_count` guard:

```cpp
  // A tap while Settings is open activates the centred item. It must NEVER fall through to
  // macros_request_fire() -- firing a macro from a menu tap is the wrong surprise on a device
  // whose whole job is sending keystrokes.
  if (ui_mode != UI_RING) {
    settings_enter_requested = true;
    return;
  }
```

- [ ] **Step 7: Route the encoder**

Replace the body of the `for(;;)` loop in `encoder_task()` after `delta` is computed:

```cpp
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
```

Note the old guard `delta != 0 && active_count > 0 && lvgl_lock(100)` is gone: requiring `active_count > 0` would make Settings unreachable on a device with no macros, which is exactly when you might need it.

- [ ] **Step 8: Own the transitions in `loop()`**

Add **after `build_settings_overlay()`** and before `build_ring_ui()` — it calls `settings_layout()`, so it must come after that definition:

```cpp
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
    lv_obj_clear_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(settings_overlay);
    if (pairing_overlay) lv_obj_move_foreground(pairing_overlay);
    lvgl_unlock();
    Serial.println("[diag] settings opened, macros stopped");
    return;
  }

  if (ui_mode == UI_RING) return;

  if (settings_enter_requested) {
    settings_enter_requested = false;
    // Task 4 turns this into "open the centred item's editor".
    Serial.printf("[diag] settings: activate item %d (%s)\n",
                  settings_sel, SETTINGS_ITEMS[settings_sel].name);
    settings_last_activity = millis();
  }

  bool timed_out = (millis() - settings_last_activity > SETTINGS_IDLE_TIMEOUT_MS);
  if (settings_close_requested || timed_out) {
    if (!lvgl_lock(200)) return;
    settings_close_requested = false;
    ui_mode = UI_RING;
    lv_obj_add_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_scr_act());
    lvgl_unlock();
    Serial.printf("[diag] settings closed (%s)\n", timed_out ? "idle timeout" : "swipe down");
  }
}
```

In `loop()`, add after `update_profiles_reload();`:

```cpp
  update_settings();
```

- [ ] **Step 9: Compile, flash, replug, capture**

Compile (exit 0), upload, **physically replug**, then capture 60 s while performing all four Step 1 observations.

- [ ] **Step 10: Commit**

```bash
git add firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino
git commit -m "feat(m7): Settings menu -- swipe up opens and kills macros, swipe down closes

Slot-machine list: the active row sits at the screen centre, so the item
your tap acts on is literally in the middle -- the same rule the ring UI
already follows.

Swipe-up stops every running macro as part of opening. That is what makes
swipe-down safe to reuse as 'close': the gesture is the kill-all panic
gesture on the ring, and overloading it would otherwise make a runaway
toggle macro unkillable behind a menu. Killing on entry removes the
conflict at its source rather than working around it.

The kill runs in loop()'s update_settings(), not the gesture callback, so
'Settings is open' and 'nothing is running' land as one transition under a
single lock acquisition.

Taps are intercepted before macros_request_fire(): a menu tap must never
fire a macro."
```

---

## Task 4: Brightness gauge editor

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino` (`build_settings_overlay`, `update_settings`, `encoder_task`, `setup`)

**Interfaces:**
- Consumes: `setting_item_t`, `SETTINGS_ITEMS`, `ui_mode`, `settings_enter_requested`, `brightness_get/apply`, `current_duty`, `brightness_dirty` (Task 3); `state_set_brightness`, `state_brightness` (Task 2); `&orbitron_14/24` (Task 1).

- [ ] **Step 1: Acceptance criterion**

1. Tap `Brightness` → a half-moon arc across the **top** of the screen with a percentage below it.
2. Turning the knob **clockwise makes the panel brighter** and fills the arc **left to right**. Both directions work.
3. Tap → back to the list. Swipe down → ring. Serial shows `[state] brightness -> N (persisted)` **exactly once**, not once per detent.
4. **Power cycle → the panel comes up at the set brightness**, and boot serial reads `duty=N` with the value you chose.
5. At minimum the screen is still readable. If it is not, raise `BRIGHTNESS_MIN` — that is what the constant is for.

- [ ] **Step 2: Split the overlay into list and edit panels**

In `build_settings_overlay()`, wrap the existing rails and rows in a panel and add the gauge panel. Add these declarations next to `settings_overlay`:

```cpp
static lv_obj_t *settings_list_panel = nullptr;
static lv_obj_t *settings_edit_panel = nullptr;
static lv_obj_t *gauge_arc   = nullptr;
static lv_obj_t *gauge_value = nullptr;
static lv_obj_t *gauge_label = nullptr;
```

Replace the rail and row creation loops so their parent is `settings_list_panel` instead of `settings_overlay`, creating it first:

```cpp
  settings_list_panel = lv_obj_create(settings_overlay);
  lv_obj_set_size(settings_list_panel, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
  lv_obj_center(settings_list_panel);
  lv_obj_set_style_bg_opa(settings_list_panel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(settings_list_panel, 0, 0);
  lv_obj_clear_flag(settings_list_panel, LV_OBJ_FLAG_SCROLLABLE);
```

Then append the gauge panel:

```cpp
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
```

If the arc renders empty on hardware, try `lv_arc_set_bg_angles(gauge_arc, 180, 359)` — LVGL normalises 360 to 0 in some paths. Verify visually; do not assume.

- [ ] **Step 3: Add the gauge refresh and commit helpers**

```cpp
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
```

- [ ] **Step 4: Wire the transitions**

In `update_settings()`, replace the `settings_enter_requested` block from Task 3:

```cpp
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
```

In the close block, commit and reset the panels — swipe-down closes from the gauge too, not just the list:

```cpp
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
```

In the open block, make sure the list panel is the one showing:

```cpp
    lv_obj_add_flag(settings_edit_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(settings_list_panel, LV_OBJ_FLAG_HIDDEN);
```
immediately before `lv_obj_clear_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN);`.

- [ ] **Step 5: Route the encoder in edit mode**

In `encoder_task()`, add a branch before the `UI_RING` one:

```cpp
    } else if (ui_mode == UI_SETTINGS_EDIT) {
      const setting_item_t *it = &SETTINGS_ITEMS[settings_sel];
      it->apply(it->get() + delta * it->step);   // live preview: the panel changes as you turn
      gauge_refresh();
      settings_last_activity = millis();
```

- [ ] **Step 6: Seed `current_duty` at boot**

In `setup()`, immediately after the `lcd_bl_pwm_bsp_init(boot_brightness)` line from Task 2:

```cpp
  current_duty = boot_brightness;  // so the gauge opens at the real value, not BRIGHTNESS_MAX
```

- [ ] **Step 7: Compile, flash, replug, capture**

Capture 90 s while walking Step 1's five observations, including a power cycle for #4.

- [ ] **Step 8: Commit**

```bash
git add firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino
git commit -m "feat(m7): on-device brightness with a half-moon gauge

Tap Brightness, turn the knob, tap to confirm. Adjustment is live so you
judge the actual panel rather than a number.

The arc spans 180..360 deg -- 9 o'clock through 12 to 3 -- so it fills
left to right as the knob turns clockwise, reusing the same angle
convention wedge_center_angle() uses for the macro ring.

BRIGHTNESS_MIN is 20, not 0, on purpose: a knob that can reach a black
screen looks bricked with no way back to the setting.

One NVS write per Settings session, on exit, from the loop task -- never
per detent, and never from the LVGL task where a flash write would stall
the renderer.

Observed on hardware: <FILL IN -- brightness after power cycle>"
```

---

## Task 5: A second default profile

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/macro_engine.cpp:22-47`

**Interfaces:**
- Produces: a default document containing two profiles — Tasks 6 and 7 cannot be exercised without it.

- [ ] **Step 1: Acceptance criterion**

Erase the config (`LittleFS` `/profiles.json` deleted, or flash erased), boot, and serial shows `[diag] profiles_reload: activeProfileIdx=0 numProfiles=2`. The ring still shows the `Editing` profile's five macros.

- [ ] **Step 2: Add the profile**

In `defaultProfilesJson`, after the `Editing` profile's closing `}` and before the `]` that ends `profiles`, add a comma and:

```json
    {
      "name": "Media",
      "color": "#30C060",
      "macros": [
        { "pos": 0, "name": "Play/Pause", "color": "#30C060", "mode": "play_once", "actions": [ { "type": "consumer", "code": "PLAY_PAUSE" } ] },
        { "pos": 1, "name": "Next", "color": "#3080E0", "mode": "play_once", "actions": [ { "type": "consumer", "code": "NEXT" } ] },
        { "pos": 2, "name": "Prev", "color": "#E0A030", "mode": "play_once", "actions": [ { "type": "consumer", "code": "PREV" } ] },
        { "pos": 3, "name": "Vol Up", "color": "#A030E0", "mode": "play_once", "actions": [ { "type": "consumer", "code": "VOL_UP" } ] }
      ]
    }
```

The codes are exactly the six `getConsumerCode()` accepts (`macro_engine.cpp:237-243`): `MUTE`, `VOL_UP`, `VOL_DOWN`, `PLAY_PAUSE`, `NEXT`, `PREV`. Anything else silently returns 0 and the macro does nothing — this is why the strings are copied rather than invented. Note they are **not** the USB HID spelling (`SCAN_NEXT`); use the firmware's names.

Leave `"version": 2`. Bumping to 3 is M8b.

- [ ] **Step 3: Compile and check the document still fits**

Compile (exit 0). Two profiles is roughly 1.6 KB serialized, well under the 8192-byte `BLE_RX_BUFFER_SIZE`, so `save_profiles` round-trips are unaffected.

- [ ] **Step 4: Flash, replug, erase config, observe**

The default is only written when `/profiles.json` is missing or unparseable, so an existing board **will not pick this up on its own**. Force it: connect the companion app and save a profile set, or erase flash before uploading:

```bash
./arduino-cli.exe upload -p <PORT> --fqbn "<FQBN>" firmware/Waveshare_LVGL_Test
```

then confirm `numProfiles=2` in the capture. If it says `numProfiles=1`, the old file survived — that is the expected fallback behaviour working correctly, not a bug, but you must clear it before Tasks 6-7 mean anything.

- [ ] **Step 5: Commit**

```bash
git add firmware/Waveshare_LVGL_Test/macro_engine.cpp
git commit -m "feat: ship a second default profile so switching is testable

The default document has always carried exactly one profile, which makes
on-device profile switching untestable on a freshly flashed board -- there
is nothing to switch to.

Consumer codes are the six getConsumerCode() actually accepts, not the USB
HID spellings; an unrecognised code silently returns 0 and the macro does
nothing."
```

---

## Task 6: Profile switching

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/macro_engine.h` (after `profiles_default_brightness`), `macro_engine.cpp` (after `profiles_active_name`, and `profiles_reload` line ~186)
- Modify: `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino` (`screen_gesture_cb`, `loop`, new `update_profile_switch` and `update_profile_toast`)

**Interfaces:**
- Consumes: `state_set_active_profile` (Task 2); a two-profile default (Task 5).
- Produces: `profiles_count()`, `profiles_active_index()`, `profiles_set_active(int)`, `profiles_active_color()`, `profile_switch_delta` — Task 7 uses all but the last for the indicators and their hot zones.

- [ ] **Step 1: Acceptance criterion**

1. Swipe left → switches to `Media`; the centre label shows `Media` for ~1.5 s then reverts to the selected macro's name. Swipe right → back to `Editing`. At the ends, further swipes do nothing (clamp, no wrap).
2. Serial shows `[state] activeProfile -> 1 (persisted)`.
3. **Power cycle → boots into the profile you left it on**, `activeProfileIdx=1` in the capture.
4. Fire `Caps Lock` so it loops, then swipe to switch → `any_running` goes 1→0, no crash, **no `rst:0x` line** in the capture. This is the H3-class regression test on a new trigger.

- [ ] **Step 2: Add the profile API**

`macro_engine.h`, after `profiles_default_brightness()`:

```c
// Active profile's "color" (e.g. "#3080E0"), "#FFFFFF" if absent. Used for the ring's
// directional profile indicators.
const char *profiles_active_color();

int profiles_count();
int profiles_active_index();

// Switches the active profile. Returns false (and does nothing) if `idx` is out of range or
// already active. Stops every running macro first and persists the new index to NVS.
//
// loop()-task ONLY, same constraint as macros_fire(): it calls macros_stop_all(), which mutates
// runningMacros[] with no locking. Every ActiveMacro also holds a JsonObject into the profile
// being switched away from, so skipping the stop is the H3 use-after-free on a new trigger.
//
// The CALLER is responsible for rebuilding the ring UI afterwards, under lvgl_lock().
bool profiles_set_active(int idx);
```

`macro_engine.cpp`, after `profiles_active_name()`:

```cpp
const char *profiles_active_color() {
  JsonArray profiles = profilesDoc["profiles"];
  if (profiles.isNull() || activeProfileIdx >= (int)profiles.size()) return "#FFFFFF";
  JsonObject prof = profiles[activeProfileIdx];
  return prof["color"] | "#FFFFFF";
}

int profiles_count() {
  JsonArray profiles = profilesDoc["profiles"];
  return profiles.isNull() ? 0 : (int)profiles.size();
}

int profiles_active_index() {
  return activeProfileIdx;
}

bool profiles_set_active(int idx) {
  if (idx < 0 || idx >= profiles_count()) return false;
  if (idx == activeProfileIdx) return false;
  macros_stop_all();
  activeProfileIdx = idx;
  state_set_active_profile(activeProfileIdx);
  Serial.printf("[diag] profile -> %d (%s)\n", activeProfileIdx, profiles_active_name());
  return true;
}
```

- [ ] **Step 3: Persist the clamp**

In `profiles_reload()`, replace line ~186:

```cpp
  if (profiles.isNull() || activeProfileIdx >= (int)profiles.size()) {
    activeProfileIdx = 0;
    // Write the correction back. Clamping in RAM only left a stale out-of-range index in NVS
    // forever, re-clamped silently on every boot -- "reading it without ever writing it is the
    // same as not having it" (spec section 8).
    state_set_active_profile(activeProfileIdx);
  }
```

- [ ] **Step 4: Add the switch request flag and the toast**

Next to the other Settings declarations in the `.ino`:

```cpp
// Assigned (never accumulated) by the gesture callback and zeroed by loop(), so there is no
// cross-core read-modify-write. A very fast double-swipe may register as one switch; acceptable.
static volatile int8_t profile_switch_delta = 0;

// A switch silently re-legends the whole ring, so the profile name is shown briefly to say what
// changed. 0 = no toast pending.
static unsigned long profile_toast_until = 0;
#define PROFILE_TOAST_MS 1500
```

- [ ] **Step 5: Handle the switch in `loop()`**

Add next to `update_settings()`:

```cpp
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
    lv_label_set_text(center_label, profiles_active_name());
    profile_toast_until = millis() + PROFILE_TOAST_MS;
  }
  lvgl_unlock();
}

static void update_profile_toast(void) {
  if (profile_toast_until == 0) return;
  if (millis() < profile_toast_until) return;
  // Leave the deadline set on lock failure so the next tick retries, or the profile name stays
  // burned into the centre label.
  if (!lvgl_lock(50)) return;
  profile_toast_until = 0;
  update_center_label(selected_idx);
  lvgl_unlock();
}
```

In `loop()`, after `update_settings();`:

```cpp
  update_profile_switch();
  update_profile_toast();
```

- [ ] **Step 6: Route the horizontal swipes**

In `screen_gesture_cb()`, after the `LV_DIR_TOP` block and before the `LV_DIR_BOTTOM` check:

```cpp
  // Horizontal swipes switch profiles, matching M5_M6_config.ino:1375-1380 so the same gesture
  // means the same thing on both boards: swipe left = next, swipe right = previous.
  // Ignored while Settings or the pairing overlay owns the screen.
  if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
    if (ui_mode == UI_RING && !ble_pairing_active()) {
      profile_switch_delta = (dir == LV_DIR_LEFT) ? 1 : -1;
    }
    return;
  }
```

- [ ] **Step 7: Compile, flash, replug, capture**

Capture 120 s covering all four Step 1 observations, including the power cycle and the running-macro switch. Grep the capture for `rst:0x` — any hit means a reset and the task is not done.

- [ ] **Step 8: Commit**

```bash
git add firmware/Waveshare_LVGL_Test/macro_engine.* firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino
git commit -m "feat(m8): on-device profile switching, and activeProfile finally gets a writer

Swipe left = next, swipe right = previous, matching the M5Dial so the same
gesture means the same thing on both boards. Clamps at both ends.

This is what completes M7. activeProfile has been READ from NVS since the
first profile store and written by nothing, which spec section 8 calls out
as 'the same as not having it' -- there was simply no code path that could
change it until now.

Also persists the clamp: an out-of-range stored index (a profile was
deleted) was corrected in RAM only and left stale in NVS, to be re-clamped
silently on every boot.

profiles_set_active() stops running macros before switching. Every
ActiveMacro holds a JsonObject into the profile being switched away from,
so skipping that is the H3 use-after-free on a new trigger.

Observed on hardware: <FILL IN -- switch under a running toggle macro, no rst:0x>"
```

---

## Task 7: Directional indicators and their tap hot zones

**Files:**
- Modify: `firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino` (`ring_draw_event_cb`, `screen_click_cb`)

**Interfaces:**
- Consumes: `profiles_count()`, `profiles_active_index()`, `profiles_active_color()`, `profile_switch_delta` (Task 6); `parse_hex_color()` (existing, line 40).

- [ ] **Step 1: Acceptance criterion**

1. On `Editing` (index 0 of 2): a triangle on the **right only**. On `Media` (index 1 of 2): **left only**. Both drawn in the active profile's own colour.
2. Tapping a visible triangle switches profiles.
3. Tapping the centre where there is **no** triangle on that side **fires the selected macro** — the tap falls through exactly as it does today.
4. Tapping the middle of the centre area always fires, never switches.

- [ ] **Step 2: Add the geometry constants**

Next to the ring constants near line 22:

```cpp
// Profile indicators live INSIDE the inner hole: the ring band is full of wedges, and outside
// RING_OUTER_R there are only 8 px on a 360 px panel. That puts them in the tap-to-fire zone,
// so their hot zones switch profiles rather than firing -- something that looks tappable inside
// the fire zone must not fire a macro. INDICATOR_CX + INDICATOR_HALF_W must stay < RING_INNER_R.
#define INDICATOR_CX      72
#define INDICATOR_HALF_W  14
#define INDICATOR_HALF_H  12
#define HOTZONE_MIN_DX    60
#define HOTZONE_MAX_DY    40
```

- [ ] **Step 3: Draw the indicators**

At the end of `ring_draw_event_cb()`, after the wedge loop:

```cpp
  // Drawn in the active profile's own colour, and only when a profile exists in that direction.
  // screen_click_cb() gates its hot zones on the same condition, so at either end of the list a
  // tap there falls through and fires instead of switching.
  int pcount = profiles_count();
  int pidx   = profiles_active_index();
  if (pcount > 1) {
    lv_draw_rect_dsc_t tri;            // lv_draw_polygon takes a RECT dsc, not an arc/tri one
    lv_draw_rect_dsc_init(&tri);
    tri.bg_color = lv_color_hex(parse_hex_color(profiles_active_color(), 0xFFFFFF));
    tri.bg_opa   = LV_OPA_COVER;
    const lv_coord_t cx = EXAMPLE_LCD_H_RES / 2;
    const lv_coord_t cy = EXAMPLE_LCD_V_RES / 2;

    if (pidx > 0) {                    // points left = previous
      lv_point_t p[3] = {
        { (lv_coord_t)(cx - INDICATOR_CX - INDICATOR_HALF_W), cy },
        { (lv_coord_t)(cx - INDICATOR_CX + INDICATOR_HALF_W), (lv_coord_t)(cy - INDICATOR_HALF_H) },
        { (lv_coord_t)(cx - INDICATOR_CX + INDICATOR_HALF_W), (lv_coord_t)(cy + INDICATOR_HALF_H) },
      };
      lv_draw_polygon(draw_ctx, &tri, p, 3);
    }
    if (pidx < pcount - 1) {           // points right = next
      lv_point_t p[3] = {
        { (lv_coord_t)(cx + INDICATOR_CX + INDICATOR_HALF_W), cy },
        { (lv_coord_t)(cx + INDICATOR_CX - INDICATOR_HALF_W), (lv_coord_t)(cy - INDICATOR_HALF_H) },
        { (lv_coord_t)(cx + INDICATOR_CX - INDICATOR_HALF_W), (lv_coord_t)(cy + INDICATOR_HALF_H) },
      };
      lv_draw_polygon(draw_ctx, &tri, p, 3);
    }
  }
```

`lv_draw_polygon(lv_draw_ctx_t *, const lv_draw_rect_dsc_t *, const lv_point_t[], uint16_t)` — verified present in the installed LVGL 8.4.0 at `src/draw/lv_draw_triangle.h:30`. Note it takes a **rect** descriptor.

- [ ] **Step 4: Make the hot zones switch**

In `screen_click_cb()`, replace the `dist < RING_INNER_R` branch:

```cpp
  if (dist < RING_INNER_R) {
    // The indicator hot zones. Live ONLY while their indicator is showing, so at either end of
    // the profile list the tap falls straight through and fires as it always has. The middle
    // ~120 px stays a comfortable fire target.
    int pcount = profiles_count();
    int pidx   = profiles_active_index();
    if (fabsf(dy) < HOTZONE_MAX_DY && fabsf(dx) > HOTZONE_MIN_DX) {
      if (dx < 0 && pidx > 0) {
        TRACE("[tap] -> indicator prev\n");
        profile_switch_delta = -1;
        return;
      }
      if (dx > 0 && pidx < pcount - 1) {
        TRACE("[tap] -> indicator next\n");
        profile_switch_delta = 1;
        return;
      }
    }
    TRACE("[tap] -> center, fire pos=%d\n", active_positions[selected_idx]);
    macros_request_fire(active_positions[selected_idx]);
  } else if (dist <= RING_OUTER_R + 10) {
```

`fabsf` needs `math.h`, already included at line 1.

- [ ] **Step 5: Compile, flash, replug, observe**

Set `DRAUPNIR_TRACE_INPUT` to `1` in `trace.h:16` for this task so the `[tap] -> indicator` lines are visible. **Set it back to `0` before committing** — `0` is the repo's resting state, and the tap tracing is verbose enough to bury everything else in a capture.

- [ ] **Step 6: Commit**

```bash
git add firmware/Waveshare_LVGL_Test/Waveshare_LVGL_Test.ino
git commit -m "feat(m8): directional profile indicators, tappable

Ported from M5_M6_config.ino:206-211: a triangle each side in the active
profile's own colour, shown only when a profile exists that way.

They have to sit in the inner hole -- the ring band is full of wedges and
outside RING_OUTER_R there are 8 px to work with -- which puts them inside
the tap-to-fire zone. So their hot zones switch profiles rather than
firing: something that looks tappable must not fire a macro instead.

A hot zone is live only while its indicator shows, so at the ends of the
list the tap falls through and fires exactly as before, and the middle
~120 px stays a comfortable fire target."
```

---

## Final verification pass

Run the whole of spec §11 in one session on one build, after Task 7. Individual task checks confirm their own change; this confirms they did not break each other.

- [ ] Every string renders in Orbitron; wedge-label truncation judged at both low and high macro counts. **Decide the §5a fallback here** — record the decision either way.
- [ ] Erase NVS → boots at the JSON seed (160).
- [ ] Settings: opens, one-item list does not scroll (expected), tap enters the gauge.
- [ ] Gauge: live panel change, arc fills left-to-right clockwise, percentage tracks.
- [ ] Exits: tap → list; swipe down from the gauge → ring; 8 s idle → closes. Brightness committed in all three.
- [ ] Power cycle → brightness persists.
- [ ] Minimum brightness is still readable. If not, raise `BRIGHTNESS_MIN` and re-verify.
- [ ] Toggle macro running + swipe up → macro stops **and** Settings opens.
- [ ] BLE `trigger` while in Settings → macro runs (the documented exception, spec §5). Swipe down, swipe down → stops.
- [ ] Profile switch: toast, indicators correct at both ends, clamp at the ends.
- [ ] Indicator taps switch; centre taps with no indicator fire.
- [ ] Toggle macro running + profile switch → stops, no crash, no `rst:0x`.
- [ ] Power cycle → boots into the last-used profile.
- [ ] Delete a profile from the app so the index is out of range → boots clamped, and **the second boot does not re-clamp** (that is what proves the write landed).
- [ ] Heap stable across the whole session — no per-cycle drift in `[diag] loop alive heap=`.
- [ ] No resets anywhere in the capture.

Then update `docs/Draupnir_Spec.md` §10 (M7 and M8 → Done, with the date) and `docs/HANDOFF.md`, recording only what was actually observed.
