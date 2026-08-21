# M7 + M8 — Persistence, Settings menu, on-device profile switching

*Design, 2026-08-07. Waveshare ESP32-S3 knob (`firmware/Waveshare_LVGL_Test/`).*

Supersedes the separate M7 and M8 rows in `docs/Draupnir_Spec.md` §10, which are collapsed into
one milestone here. Also carries one item not in either: the UI moves to **Orbitron** (§5a),
matching the M5Dial firmware and the companion app.

---

## 1. Why these two are one milestone

Spec §10 lists them separately:


| #   | Milestone                                                                          |
| --- | ---------------------------------------------------------------------------------- |
| M7  | **Persistence** — write `activeProfile` + brightness to NVS and honor them at boot |
| M8  | **On-device profile switching** with directional indicators                        |


M7 alone cannot be finished. Spec §8 states the requirement as *"written on change and restored on
boot — reading it without ever writing it is the same as not having it,"* and today
`macro_engine.cpp:183` reads `activeProfile` from NVS while **nothing anywhere writes it**. The only
thing that would ever change it is a profile switch, which is M8. Shipping M7 by itself would add an
unreachable setter and leave the defect the milestone exists to close.

They also share one piece of machinery — a modal overlay driven by the encoder with a loop()-task
hand-off — so building them together avoids writing that twice.

## 2. Current state


| Concern                | Today                                                                                                                                              |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| `activeProfileIdx`     | read from NVS at `macro_engine.cpp:183`, **never written**; clamped in RAM only                                                                    |
| brightness             | `settings.brightness: 160` in the default JSON, **never applied**; `Waveshare_LVGL_Test.ino:503` hardcodes `lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255)` |
| runtime brightness API | `setUpdutySubdivide(uint16_t duty)` — exists, 0–255, unused                                                                                        |
| NVS owner              | `static Preferences prefs` in `macro_engine.cpp`, namespace `draupnir`                                                                             |
| profile switching      | none                                                                                                                                               |
| app brightness UI      | **none** — confirmed with the owner                                                                                                                |


## 3. Storage model

**NVS is authoritative. `profiles.json` is seed-only.**

```
boot:   brightness    = NVS["brightness"]    ?? json.settings.brightness ?? 160
        activeProfile = NVS["activeProfile"] ?? 0

writes: brightness    <- on leaving the brightness editor, if changed
        activeProfile <- on profile switch, and on clamp (§7)
```

Nothing ever copies a JSON value back over a written NVS value.

**Why not adopt `settings.brightness` on `save_profiles`:** the companion app has no brightness
control, so it round-trips whatever it fetched. Adopting on save would mean *every macro edit*
writes the stale `160` over the brightness you just set on the knob. The identical argument applies
to `activeProfile` once §7 ships a device-side writer: an app save carrying a stale index would
silently revert your profile switch.

**Why NVS and not just JSON** (which also persists): a profile switch is a knob gesture. Rewriting a
~900 byte JSON per switch means flash wear and a race against the `save_profiles` path that H3/H4
just hardened. NVS is the right home for small values the device itself mutates — which is why
`activeProfile` has lived there since the initial MVP (`69b1034`), predating the BLE pivot.

## 4. Module boundaries

### New — `device_state.{h,cpp}`

NVS ownership moves out of `macro_engine.cpp`, which owns `Preferences` today only because it
happened to need `activeProfile`. Brightness is a display concern and does not belong in the macro
engine.

Named `device_state` rather than `settings` because **"Settings" is now a user-facing screen name**
(§5). The name matches spec §8's own wording: *"**State:** last profile + brightness in NVS."*

```c
void    state_init();                         // prefs.begin("draupnir", false)
int     state_active_profile(int fallback);
void    state_set_active_profile(int idx);
uint8_t state_brightness(uint8_t fallback);
void    state_set_brightness(uint8_t duty);
```

Deliberately free of LVGL, `ledc`, and JSON. Callers pass the seed in as `fallback`; the `.ino` does
the actual `setUpdutySubdivide()`. This keeps the spec §3 shared-layer boundary intact — the M5Dial
can adopt this file unchanged and swap in `M5Dial.Display.setBrightness()`.

NVS keys stay under the existing `draupnir` namespace: `activeProfile` (int, existing),
`brightness` (uchar, new). Both are within NVS's 15-character key limit.

### Changed — `macro_engine.{h,cpp}`

Drops `#include <Preferences.h>` and `static Preferences prefs`. `profiles_init()` calls
`state_init()` in place of `prefs.begin()`.

```c
int     profiles_count();
int     profiles_active_index();
bool    profiles_set_active(int idx);      // loop()-task ONLY
uint8_t profiles_default_brightness();     // json.settings.brightness | 160
```

`profiles_set_active()` returns false and does nothing if `idx` is out of range or already active.
Otherwise it calls `macros_stop_all()`, sets `activeProfileIdx`, and persists via
`state_set_active_profile()`.

**It is loop()-task only**, for the same reason `macros_fire()` is: `macros_stop_all()` mutates
`runningMacros[]` unlocked. This must be documented in the header alongside the existing constraints.

### Changed — `Waveshare_LVGL_Test.ino`

Boot-applies brightness; adds the Settings overlay, the brightness editor, profile switching, the
directional indicators, and the profile-name toast.

### New — `orbitron_12.c`, `orbitron_14.c`, `orbitron_24.c`, and `lv_conf.h`

Generated LVGL fonts (§5a) plus the `LV_FONT_CUSTOM_DECLARE` / `LV_FONT_DEFAULT` changes that
declare them and retire `&lv_font_montserrat_14` as the default.

## 5. Settings menu

**Swipe up opens Settings and stops every running macro. Swipe down closes it.**

A vertical list that scrolls like a slot machine: the active item sits at the centre of the screen,
neighbours above and below are dimmed. The encoder scrolls; a centre tap activates the centred item.

One row for now — `Brightness` — so the list opens like this:

```
       ---------------
          Brightness          <- active: centred, 24px, full white
       ---------------
```

With a single item the list is **degenerate — it cannot scroll**, and the encoder does nothing while
the list is showing. That is expected, not a fault; noted so it is not chased as a bug during §11.

This is coherent with the ring UI's existing rule that **a centre tap acts on whatever is
selected** — here the selection is literally in the centre of the screen.

The list does not wrap: scrolling stops at the first and last item, matching the clamp-don't-wrap
choice §7 makes for profiles.

### Why opening Settings kills running macros

It is what makes `swipe down = close` safe. Swipe-down is the kill-all panic gesture on the ring,
and an earlier draft of this design kept it as kill-all *inside* Settings too, on the grounds that a
runaway `toggle` macro spamming the host must never become unkillable behind a menu. Killing on
entry removes the conflict at its source: by the time Settings is open there is nothing left to
kill, so swipe-down is free to mean "close".

It is also right on its own terms. Opening Settings is a deliberate stop-what-you-are-doing action,
and the gesture that reaches it is the one your thumb already knows for "make it stop".

**One exception, stated precisely.** Local input cannot start a macro while Settings is open — taps
route to the menu and the encoder scrolls — but the BLE `trigger` command still can
(`ble_engine.cpp:257`), and it is not gated on UI state. A bonded central deliberately test-firing
while the owner is in Settings would leave a macro running behind the menu. Recovery is one extra
gesture: swipe down to close, swipe down again to kill. Left ungated on purpose — silently
rejecting the app's test-fire because a menu happens to be open is the worse failure.

### Emphasis

Active row at 24 px in the heavier weight, full white; neighbours at 14 px regular, ~40 % opacity.
Size, weight, and contrast all carry the hierarchy. See §5a — switching to Orbitron is what makes a
real weight contrast available; LVGL's stock Montserrat set has no bold face and no synthetic bold.

### Items are a table, not hand-written screens

Other settings will use the same knob-scroll-then-tap interaction, so items are declared as data
rather than as bespoke screens:

```c
typedef struct {
  const char *name;
  int         min, max, step;
  int  (*get)(void);        // current value
  void (*apply)(int);       // live preview while the knob turns
} setting_item_t;
```

`Brightness` is the only entry today. With the `Close` row gone (swipe-down closes), every item is
now a knob-adjusted value and the table needs no kind discriminator — an earlier draft carried a
`SETTING_RANGE` / `SETTING_ACTION` enum that would now have exactly one member in use.

**Extension point:** a non-numeric setting — an M10 buzzer toggle, say — needs a way to render `0`
and `1` as `Off` and `On`. Add an optional labels array at that point, when there is a real second
item to design against. Not speculated on here.

## 5a. Typography — Orbitron

The UI moves from Montserrat to **Orbitron**, restoring the face the M5Dial build uses
(`M5_M6_config.ino:172` and seven other sites) and that the companion app already uses throughout
(`dashboard_screen.dart`, `GoogleFonts.orbitron`). It is the only element of the three-surface look
the Waveshare build does not share.

### This is a font generation, not a config flag

`fonts::Orbitron_Light_24` is an **M5GFX built-in**. The Waveshare build is LVGL and cannot reach
M5GFX's font table. Orbitron must be converted into LVGL's C-array format from the TTF:

```
npx lv_font_conv --font Orbitron-Regular.ttf --size 14 --bpp 4 \
    --format lvgl --range 0x20-0x7F -o firmware/Waveshare_LVGL_Test/orbitron_14.c
```

Node 24 / npm 12 are present, so `npx lv_font_conv` runs without a global install. Three sizes,
mapping one-for-one onto what the UI uses today so no element changes size as a side effect of the
font change:


| Size | Used by                                         |
| ---- | ----------------------------------------------- |
| 12   | wedge labels (unchanged from today)             |
| 14   | dimmed Settings rows, gauge label, centre label |
| 24   | Settings active row, gauge value                |


Range `0x20-0x7F` — ASCII only; macro names outside it already do not render today.
`--bpp 4` for antialiasing on a 360×360 AMOLED. Roughly 10–25 KB per size in flash.

Keeping wedge labels at 12 matters: Orbitron is already wider than Montserrat (see below), and
bumping the size as well would compound the truncation instead of isolating one variable.

Each generated `.c` is committed to the sketch directory; `LV_FONT_CUSTOM_DECLARE` in `lv_conf.h`
declares them, and `LV_FONT_DEFAULT` moves off `&lv_font_montserrat_14`.

### Three things to settle at generation time

1. **Weight.** M5GFX bundles *Orbitron Light*, from an older static release. Current Google Fonts
 Orbitron is a variable font spanning **400–900 with no Light (300)**. Verify what the obtained
 TTF actually offers. If Light is unavailable, Regular is the closest and the Waveshare text will
 read slightly heavier than the M5Dial's — acceptable, but it is a real difference, not parity.
2. **Weight contrast for §5.** If a heavier cut (Bold/Black) is obtainable, generate it at 24 px and
 use it for the Settings active row. This is a genuine improvement over the size-and-opacity
 workaround Montserrat forced, and is why §5's constraint was rewritten.
3. **Licence.** Orbitron is **SIL OFL 1.1**, not MIT. In an MIT-licensed open-source repo the OFL
 text and the font's copyright notice ship alongside the generated `.c` files. Note it in the
 repo's licence documentation — the OFL permits this, but only with the notice retained.

### Known cost — Orbitron is wide

Orbitron is a geometric display face with noticeably wider glyphs than Montserrat. Wedge labels are
width-constrained to the chord at `RING_MID_R` and truncate with `LV_LABEL_LONG_DOT`, so **the same
macro name will truncate sooner**, and worse as macro count climbs and wedges narrow.

Ship Orbitron everywhere first and judge it on the panel. If wedge labels read badly, the documented
fallback is Orbitron for the chrome — centre label, Settings, gauge, passkey — and Montserrat 12 for
the wedge labels only. That split is defensible typographically (display face for headline text,
neutral face for dense small text), but it is a fallback, not the target. Decide from the hardware,
not from this document.

## 6. Brightness editor

Tapping `Brightness` opens a **half-moon gauge across the top** — a 180° arc sweeping from 9
o'clock through 12 to 3 o'clock, filling left to right as the value rises. Clockwise fill matches
clockwise knob rotation, and it reuses the macro ring's own 12-o'clock origin and clockwise
convention, so the gauge reads as the same object the rest of the UI is built from.

```
        ╭───────────────╮
      ╭─╯               ╰─╮
     │   ▄▄▄▄▄▄▄▄▄░░░░░░   │   180° arc, fills L->R
    │  ╱               ╲   │
    │                      │
    │         72%          │   value, 24px
    │                      │
     │     Brightness      │   label, 14px
      ╲                   ╱
        ╰───────────────╯
```

The encoder adjusts duty **live** via `setUpdutySubdivide()`, so you judge the actual panel rather
than a number. A tap commits and returns to the list.


| Constant          | Value | Reason                                                                                                                |
| ----------------- | ----- | --------------------------------------------------------------------------------------------------------------------- |
| `BRIGHTNESS_MIN`  | 20    | **Deliberately not 0.** A knob that can be turned to a black screen looks bricked and leaves no way to find it again. |
| `BRIGHTNESS_MAX`  | 255   | LEDC 8-bit full duty                                                                                                  |
| `BRIGHTNESS_STEP` | 16    | ~15 detents min→max — enough resolution, few enough to cross quickly                                                  |


Displayed as `(duty - MIN) * 100 / (MAX - MIN)` percent.

Duty steps are **linear**, so the low end will feel perceptually coarse. Gamma correction is a
polish item, not this milestone.

### Exits

| Gesture | Behaviour |
|---|---|
| **swipe down** | close Settings; commit brightness if changed |
| 8 s idle | close Settings; commit brightness if changed |
| tap (in editor) | commit, return to the list |
| swipe up | ignored — Settings is already open |
| swipe left/right | ignored while Settings is open |

**Swipe down closes from anywhere in Settings**, list or editor, rather than backing out one level
at a time. With a single item a level-at-a-time model would cost two swipes to leave the only
screen there is. Revisit when a second setting lands and there is a list worth returning to — the
choice is between "swipe down always leaves" and "swipe down goes back one level", and only the
second is worth the extra gesture once the list is real.

This is safe only because **swipe-up killed every running macro on the way in** (§5). Swipe-down is
the panic gesture on the ring; it can be reused as "close" here precisely because nothing can be
running behind the menu.

**One NVS write per Settings session**, on exit, from the loop task — never per detent, never on the
LVGL task, where a flash write would stall the renderer.


## 7. Profile switching

**Swipe left = next, swipe right = previous**, matching `M5_M6_config.ino:1375-1380` so the same
gesture means the same thing on both boards. Clamps at both ends; no wrap. Ignored while Settings or
the pairing overlay is open.

The switch runs on the **loop task**, never in the gesture callback: it calls `macros_stop_all()`
(loop-only) and `rebuild_ring_layout()` (needs `lvgl_lock()`). The gesture sets a delta flag and
`update_profile_switch()` in `loop()` does the work under the lock, the same hand-off
`update_profiles_reload()` already uses.

Stopping macros before the switch is **not optional**: `ActiveMacro` holds `JsonObject` references
into the previous profile. This is the H3 defect class on a new trigger.

On switch, the centre label shows the **profile name for ~1.5 s**, then reverts to the selected
macro's name — otherwise the ring silently re-legends and nothing tells you what changed.
`selected_idx` resets to 0, as `update_profiles_reload()` already does.

### Clamp persistence

If the stored index is out of range (a profile was deleted), `profiles_reload()` currently clamps to
0 **in RAM only**, leaving the stale index in NVS forever to be re-clamped on every boot. The
clamped value now gets written back.

## 8. Directional indicators

A small triangle on each side, drawn in the active profile's own `color`, shown only when a profile
exists in that direction — ported from `M5_M6_config.ino:206-211`. Drawn in `ring_draw_event_cb`
alongside the wedges.

### Placement forces the hot-zone decision

They must sit in the inner hole: the ring band is full of wedges, and outside `RING_OUTER_R` (172)
there are only 8 px on a 360 px panel. But the inner hole is the **tap-to-fire** zone, and an
indicator that looks tappable but instead fires a macro is the wrong surprise on this device.

**So the indicator hot zones are tappable, and they switch profiles.**

```
hot zone:  dist < RING_INNER_R  &&  |dx| > 60  &&  |dy| < 40
           left -> previous, right -> next
fire zone: dist < RING_INNER_R  &&  not in a live hot zone
```

A hot zone is live **only when its indicator is showing**, so at either end of the profile list the
tap falls straight through and fires exactly as it does today. The centre keeps a comfortable
~120 px fire target.

## 9. Threading

Every rule in CLAUDE.md and spec §5 holds. Restated for what this milestone adds:

| Runs on | Does |
|---|---|
| `screen_gesture_cb` (LVGL task) | sets flags only — `settings_requested`, `settings_close_requested`, `profile_switch_delta` |
| `screen_click_cb` (LVGL task) | routes taps; **when Settings is open it must return without firing a macro** |
| `encoder_task` | routes by mode: ring select · list scroll · live brightness. All under `lvgl_lock()`, as today. |
| `loop()` | `update_settings()` and `update_profile_switch()` own every mode transition, the idle timeout, the NVS write, `macros_stop_all()`, and `rebuild_ring_layout()` |

`current_duty` is read and written only under `lvgl_lock()`, so no new lock is introduced.

**The kill-on-open happens in `loop()`, not the gesture callback.** `update_settings()` calls
`macros_stop_all()` directly — it is on the loop task, so it may — in the same step that raises the
overlay. Routing it through `macros_request_stop_all()` from the callback would work, but would let
the kill and the mode change land in either order, and would still kill macros in the case where the
overlay never opens because `lvgl_lock()` timed out. Doing both under one lock acquisition keeps
"Settings is open" and "nothing is running" a single transition rather than two racing ones.

**Lock-failure handling follows the H5 lesson:** acquire the lock *before* consuming a pending
transition. A lock timeout must leave the flag set so the next tick retries — committing first meant
a single 50 ms timeout consumed the transition permanently.

`profile_switch_delta` is assigned (not accumulated) by the gesture callback and zeroed by `loop()`,
avoiding a cross-core read-modify-write. A very fast double-swipe may register as one switch;
acceptable, and documented at the declaration.


### Overlay z-order

`rebuild_ring_layout()` currently re-asserts `pairing_overlay` foreground after re-creating labels.
It must now re-assert **both**, settings first and pairing last, so pairing always wins.

### API verification

Per CLAUDE.md hard constraint #4, verify `lv_draw_polygon` / triangle and arc drawing signatures
against the **installed** LVGL 8 headers before use. This codebase has already been bitten by APIs
that compile and silently do nothing.

## 10. Prerequisite — a second default profile

`defaultProfilesJson` in `macro_engine.cpp` ships **one** profile (`Editing`). Profile switching is
untestable against it out of the box. Add a second (e.g. `Media`, 3–4 macros) so a fresh device can
exercise §7 without app setup.

## 11. Hardware verification

Per CLAUDE.md, each item is confirmed on hardware; nothing is claimed that was not observed.

**Typography**

1. Boot → **every string renders in Orbitron**, none missing or boxed. Check the ring's wedge labels
   specifically: confirm how much earlier they truncate than with Montserrat, at both a low macro
   count (fat wedges) and a high one (narrow wedges). This decides whether §5a's fallback is needed.

**Brightness + persistence**

2. Erase NVS, boot → brightness is the JSON seed (160), not 255.
3. Swipe up → Settings opens; centre item legible at 24 px and clearly distinct from a dimmed
   neighbour. With one item the list cannot scroll — confirm that, rather than treating it as a
   dead encoder (§5).
4. Tap `Brightness` → gauge; encoder changes the **panel** live; percentage tracks; the arc fills
   left-to-right as the knob turns clockwise.
5. Tap → returns to the list. Swipe down → ring.
6. Swipe down from inside the **gauge** → also returns to the ring, brightness committed.
7. Leave Settings idle 8 s → closes on its own, brightness committed.
8. **Power cycle → brightness persists.**
9. Turn brightness to minimum → screen is still readable. This is what `BRIGHTNESS_MIN = 20` exists
   for; if it is not readable, raise the floor.

**Kill-on-open (§5)**

10. Fire the `Caps Lock` toggle so it loops, then **swipe up** → macro stops *and* Settings opens.
    Confirm both, not just the overlay.
11. While in Settings, fire a macro over BLE `trigger` → it runs (the documented exception). Swipe
    down, swipe down again → it stops.

**Profile switching**

12. Swipe left/right → profile switches; toast shows the name for ~1.5 s then reverts; indicators
    appear and vanish correctly at both ends of the list.
13. Tap each indicator → switches. Tap centre with no indicator on that side → **fires**.
14. Fire the `Caps Lock` toggle so it loops, then swipe to switch → macro stops, no crash, no reset.
    This is the H3-class regression test on a new trigger.
15. **Power cycle → boots into the last-used profile.**
16. Delete a profile from the app so the stored index is out of range → boots clamped **and NVS is
    corrected**. Verify the *second* boot does not re-clamp — that is what proves the write landed.

**Throughout**

17. Heap stable across the whole session — no per-cycle drift.
18. No resets. Capture with `scripts/serial_capture.ps1`, not `arduino-cli monitor`.


## 12. Out of scope

- Gamma-corrected brightness curve.
- A brightness control in the companion app, and any new BLE command for it.
- Non-ASCII glyph coverage. The generated Orbitron range is `0x20-0x7F`, matching what renders today.
- M8b (`pos` uncapping, schema v3), M9 icons, M10 polish items.
- M5Dial parity — deferred by the board sequencing in spec §3. `device_state.{h,cpp}` is written to
port unchanged.
- The unrun M6 tests (H2 overflow-recovery, H4 corruption-recovery, heap soak). Still open, tracked
in `docs/HANDOFF.md` §3.

