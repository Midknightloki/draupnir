# Project Draupnir — Smart-Knob HID Macro Controller

**A self-contained USB-HID macro controller built around a round touch screen and a rotary encoder.**

*A FORGE project spec · L0k1.Net · v3, drafted 2026-07-24*

> Codename note: *Draupnir* is the golden ring the dwarf Eitri forged that drips eight new rings every ninth night. A round controller that multiplies your macros felt apt.

---

## Changelog — what v3 changes and why

v2 specified two pucks (M5Dial + NeoTrellis 4x4) configured from a browser over Wi-Fi. Field
testing changed three things:

| Change | Reason |
|---|---|
| **Knob is the whole device.** The 16-key pad drops from *core* to *optional add-on*. | The encoder is more tactile than expected and the ring UI gives good random access on its own. The keys solve a problem the knob turned out not to have. |
| **The Wi-Fi web config UI is cut entirely — not deferred, not implemented.** | The Companion App does the job better, and it makes the on-device HTTP server, captive portal, and Wi-Fi credential flow pure liability. |
| **Standard BLE pairing with a displayed PIN replaces the bespoke token scheme.** | The hand-rolled `pairingToken` in NVS reimplemented, badly, what BLE bonding already does correctly. Passkey display + bond is the supported path. |
| **Waveshare knob becomes the primary board; M5Dial stays supported.** | The Waveshare 360x360 build is what's proven on hardware now. The M5Dial firmware still works and stays a maintained second target. |
| **Macro count per profile is no longer capped at 16.** | `pos` was a physical grid slot when there were 16 physical keys. Without them it becomes a stable identifier and ring-order key. |

Everything below is v3 and supersedes v2. The two-puck design is preserved as **Appendix B** —
it is still the origin of the data model and still the path if the key pad returns.

---

## 0. North Star & roadmap

**End goal:** a polished, standalone **smart-knob macro controller** — a round screen and a
quality detented encoder in a compact desk puck, driverless over USB HID, configured from a
phone. Distinctive because it is *complete at knob scale*: no key pad required.

The 16-key ring is now an **optional expansion**, not the destination. That is a real
simplification: one enclosure, one cable, no I2C peripheral, no per-key LED current budget.

| Phase | Build | Purpose |
|---|---|---|
| **P1 — POC (now)** | Waveshare ESP32-S3 knob (primary) + M5Dial (second target) | Prove firmware + interaction on off-the-shelf enclosed parts. No fab. |
| **P2 — Personal knob** | Custom PCB: ESP32-S3 module, round screen, quality detented encoder, machined/printed knob + enclosure | One-off "real" device; validate feel and manufacturing. |
| **P3 — Product (stretch)** | Refined P2 + DFM, enclosure tooling, packaging, docs | Small-batch sellable unit. |
| **Optional, any phase** | 16-key RGB pad as a companion puck / premium SKU | Only if a real workflow demands more than the ring gives. |

**Design constraints to honor from P1 so P3 stays open:**
- Build around a **pre-certified ESP32-S3 module** so radio FCC/CE modular approval is largely
  inherited — turns certification from a blocker into a checkbox.
- Keep **firmware + hardware open source** (duckyPad playbook) — values fit *and* community flywheel.
- Likely lanes: **Tindie** (sell-as-you-fab) and/or **Crowd Supply**.
- Keep the macro data model and Companion App **hardware-agnostic** so profiles survive
  form-factor changes — and so the two P1 boards, which differ in screen size and driver stack,
  share one schema and one app.

---

## 1. Concept

One puck. A round touch screen with a detented rotary encoder around/behind it.

The screen renders the active profile's macros as a **ring of colored wedges** — one wedge per
macro, sized to fill the ring, so four macros means four fat wedges rather than four slivers and
twelve empty slots. Rotating the encoder moves the selection wedge by wedge; the center of the
screen names the selected macro.

**Three ways to fire a macro:**
- **Rotate to it, tap the center** — the knob-native path.
- **Tap its wedge directly** — random access, no scrolling.
- **Trigger it from the Companion App** — for testing and remote use.

It runs as a standard USB HID device — no host software at runtime. Macros live in the device's
flash. Configuration is a **BLE Companion App**.

### What it is
A desk macro knob: screen + encoder + touch, fully enclosed, driverless, no 3D printing required.

### What it is not (v1 non-goals)
- Not a duckyScript interpreter (sequences + delays, no loops/variables on-device).
- **No web UI, no on-device HTTP server, no Wi-Fi provisioning.** Cut, permanently.
- **No bespoke pairing tokens.** BLE bonding is the security boundary.
- No microSD — profiles live in internal flash.
- No physical key pad in the base device.

---

## 2. Goals & success criteria

1. Plug-and-play **USB HID** keyboard/mouse/media device — works at boot, no drivers.
2. **Ring UI gives random access** to every macro in the active profile: rotate-and-fire, or tap
   a wedge directly.
3. **Color legend:** each wedge drawn in its macro's color; the selected macro is highlighted and
   named in the center; **its icon renders on the wedge.**
4. **Multiple profiles**, each holding a variable number of macros, persisted in onboard flash;
   switching profiles re-legends the ring. Profile switching is reachable **on-device**, not only
   from the app.
5. Macros support **key combos, typed text, media/consumer keys, basic mouse, and inter-step delays.**
6. Configurable over **BLE from the Companion App**, secured by **standard BLE pairing with a
   passkey displayed on the device screen**.
7. **Survives power cycles:** boots into the last-used profile, at the saved brightness, with the
   right colors.
8. **Stays trustworthy:** a malformed or interrupted config write never bricks the device, and an
   unpaired BLE central cannot read or modify profiles or inject keystrokes.

**Done when:** I configure a profile in the app, unplug/replug, see the ring light in its colors
with the right profile selected, tap a wedge, and the keystrokes land in the focused app — with
the phone nowhere nearby.

---

## 3. Hardware

### Primary target — Waveshare ESP32-S3 knob (1.8" round AMOLED)

Values below are taken from the working firmware in `firmware/Waveshare_LVGL_Test/`; that code
is the authority if a datasheet disagrees.

| Item | Detail |
|---|---|
| Controller | **ESP32-S3R8** (QFN56, rev v0.2), native USB (OTG/CDC) |
| Flash | **16 MB**, quad (4 data lines) per eFuse, 3.3 V |
| PSRAM | **8 MB embedded** (AP_3v3, octal) — **present but currently unused**, see below |
| microSD | **TF-018, 4-bit SDMMC** — GPIO 2/3/4/5/6/42. Not SPI; use `SD_MMC`. Unused by the firmware |
| Haptics | **DRV2605L** driving an **LRA** via pads PP1/PP2, on the touch I2C bus. See `docs/Waveshare_Hardware_Reference.md` §5 |
| Second MCU | **ESP32-U4WDH** (4 MB flash), own antenna, crystal and USB-UART bridge. **Connected to the S3 by a dedicated UART** — S3 GPIO48/38 to U4WDH IO23/IO18. Owns the board's second encoder. Unused by Draupnir |
| Display | 1.8" round AMOLED, **360x360**, **SH8601** over **QSPI**, 16 bpp |
| LCD pins | CS 14, PCLK 13, D0-D3 15/16/17/18, RST 21, backlight 47 (LEDC PWM) |
| Touch | **CST816**, I2C addr **0x15**, SDA 11 / SCL 12 — **shared with the DRV2605 haptic driver** |
| Encoder | Rotary, A = **GPIO 8**, B = **GPIO 7** |
| Encoder button | **None exists.** SW2 is an SSCM110100 — a four-pin encoder with no shaft switch (schematic sheet 1). A "press the knob" gesture must use the touchscreen. |
| UI stack | LVGL + `esp_lcd_sh8601` |
| FQBN | See `docs/Toolchain_arduino-cli.md` — generic `esp32:esp32:esp32s3`, Espressif core required |

**On PSRAM.** The board has 8 MB and the firmware uses none of it: LVGL draw buffers are allocated
`MALLOC_CAP_DMA` from internal RAM, and the build currently sets `PSRAM=disabled` to keep the
memory configuration out of the M6 stability diagnosis. So heap is tight **by choice, not by
constraint** — unlike the M5Dial, where it is a hard limit. Enabling `PSRAM=opi` is the obvious
relief valve for the BLE reassembly buffer and the profile `JsonDocument` once M6 is stable.

**Two board quirks that cost real time if met cold** (both detailed in the toolchain doc):
the **USB-C plug orientation** selects which of the two MCUs the single USB port reaches — plug
it the wrong way and you are talking to the ESP32-U4WDH, not the S3. The mechanism is passive:
USB-C carries two D+/D- pairs and CN1 wires one to each chip. *(Previously documented here as a
CH445P analog switch; that part switches I2S audio — see
`docs/Waveshare_Hardware_Reference.md` §7.)* And **auto-reset does not work**: the running firmware's TinyUSB CDC ignores esptool's
DTR/RTS reset, so download mode requires a manual BOOT press.

### Retired target — M5Stack Dial v1.1 *(retired 2026-09-23)*

**Not a shipping target.** The M5Dial is not sold, not supported, and receives no further work.
It is **frozen for the owner's personal use**: `firmware/M5_M6_config/` builds, runs, and stays on
the current schema. The Waveshare is self-contained and orderable wholesale; the M5Dial is neither
economical to build nor to ship at scale, which is what decided it.

Frozen is not the same as gone, and the difference is load-bearing: a device the owner still uses
receives no firmware updates, so **every shared-layer change must be additive** (see below).


| Item | Detail |
|---|---|
| Controller | M5StampS3 — ESP32-S3FN8, native USB |
| Flash | 8 MB (**no PSRAM, no SD**) |
| Display | 1.28" round IPS, GC9A01, 240x240, FT3267 touch |
| Input | Rotary encoder (16 detents / 64 PPR) + knob button |
| Extras | Buzzer, RTC; PORT.A (Grove I2C, G13/G15), PORT.B (GPIO, G2/G1) |
| Download mode | Hold **G0** on the back Stamp, plug USB-C, release |

### Sequencing: Waveshare only *(decided 2026-07-26, superseded 2026-09-23)*

**Superseded.** The original plan was to polish the Waveshare through M10, then circle back and
bring the M5Dial up to spec. The M5Dial is now retired (above), so step 2 never happens and **no
port is owed.** New work targets the Waveshare.

What survives the retirement is the rule that sequencing was there to protect, reframed from a
scheduling constraint into a **compatibility** one:

> **Shared-layer changes must be additive.** The schema, the BLE protocol, the macro engine and
> the exported file format are still read by a frozen M5Dial that will never be updated. Removing
> a field, repurposing one, or changing what an existing value means breaks a device in daily use
> with no fix available to it. Add; do not take away.

`icon_xbm` is the worked example: M12 adds a higher-resolution icon path for the Waveshare and
keeps sending the old 18×18 bitmap untouched, precisely because the frozen board consumes it. See
`docs/superpowers/specs/2026-09-23-waveshare-icon-rendering-design.md` §2.1.

### The shared/board-specific boundary

Written when two boards were shipping; still the right boundary, and now the thing that keeps the
frozen M5Dial working. The shared layer — schema, BLE protocol, macro engine, Companion App — is
consumed by both, so it stays board-agnostic and additive. Only the display/input/driver layer is
Waveshare-specific, and that is where M12's glyph rendering lives. Concretely, the boundary is:

- **Shared, board-agnostic:** macro engine, profile store, BLE command handling, action types.
  These are already factored out on the Waveshare side (`macro_engine.*`, `ble_engine.*`) and
  that factoring is the model — the M5Dial sketch should converge on it rather than the reverse.
- **Per-board:** screen driver + resolution, touch driver, encoder wiring, UI geometry
  (240x240 vs 360x360), and any board-specific peripherals.

Screen geometry must be derived from the resolution constants, never hardcoded, so the same ring
UI renders on both. A change to the macro engine that requires touching both sketches separately
is a sign the boundary has leaked.

### Optional expansion — 16-key RGB pad

Not part of the base device. If revisited: Adafruit NeoTrellis 4x4 (seesaw, I2C 0x2E, STEMMA
JST-PH — needs a Grove adapter on the M5Dial) or 4x NeoKey 1x4 for hotswap mechanical. The data
model already accommodates it — see §6 on `pos`.

---

## 4. Interaction model

**Run mode (the ring):**
- **One wedge per macro**, sized `360 / count`, drawn in the macro's color with its icon.
- **Center** shows the selected macro's name, with the profile name above it.
- **Rotate** -> selection moves one wedge per detent; screen feedback must match tactile feedback
  (see §13 — this has been a real defect).
- **Tap a wedge** -> select it and fire it.
- **Tap the center** -> fire the current selection without changing it.
- **Tap outside the ring** (bezel) -> ignored.
- **Switch profiles on-device** -> swipe left/right, with directional markers showing that other
  profiles exist; no wrap-around at the ends.
- **Pairing overlay** -> full-screen, shows the BLE passkey while pairing is in progress.

**Optional per profile:** dial-as-volume (turn = volume; tap still fires).

---

## 5. Firmware architecture (Arduino, ESP32-S3)

```
+-----------------------------------------------+
| App / state machine (run <-> pairing)         |
+---------------------+-------------------+-----+
| UI layer            | Input layer       | HID |
| LVGL ring, wedges,  | encoder, touch    | Tiny|
| icons, overlays     |                   | USB |
+---------------------+-------------------+-----+
| Macro engine (executes action sequences)      |
+-----------------------------------------------+
| Store: LittleFS (profiles.json) + NVS (state) |
+-----------------------------------------------+
| BLE GATT config transport (chunked + ack)     |
+-----------------------------------------------+
```

Note what is **absent** versus v2: no `ESPAsyncWebServer`, no `AsyncTCP`, no captive portal, no
Wi-Fi stack. Removing it also removes the RAM and boot-time cost of bringing up Wi-Fi alongside
BLE and USB on a board with no PSRAM.

**Key libraries**
- **Waveshare:** LVGL + `esp_lcd_sh8601` + CST816 driver + `bidi_switch_knob`.
- **M5Dial:** `M5Dial` / `M5Unified` (M5GFX).
- Arduino ESP32 USB: `USBHIDKeyboard`, `USBHIDMouse`, `USBHIDConsumerControl` (TinyUSB).
- `LittleFS` (macro store) + `Preferences`/NVS (last profile, brightness).
- `ArduinoJson`; ESP32 BLE (NimBLE-backed on the current core).

**Threading rules** — these are load-bearing and have already caused shipped bugs:
- **HID interfaces register before `USB.begin()`.**
- **The macro engine is loop()-task only.** UI callbacks (touch/encoder) run on the LVGL task and
  must enqueue via `macros_request_fire()`, never call `macros_fire()` directly.
- **LVGL objects are touched only under `lvgl_lock()`**, from loop() or the LVGL task.
- **BLE callbacks run on the BLE host task** and must not touch LVGL or the macro engine
  directly; they hand off through queues and flags drained in loop().
- **A profile reload must stop all running macros first** — the macro engine holds `JsonObject`
  references into the profile document, which reloading invalidates.

**Main loop**
1. Drain the fire queue and advance running macros.
2. Drain the BLE command queue.
3. Sync UI overlays (pairing) and rebuild the ring if profiles changed.

---

## 6. Macro data model

`profiles.json` in LittleFS. **Variable macro count** — a profile holds as many macros as it has,
and the ring divides itself accordingly.

```json
{
  "version": 3,
  "activeProfile": 0,
  "settings": { "brightness": 160, "buzzer": true, "orientation": 0 },
  "profiles": [
    {
      "name": "Editing",
      "color": "#3080E0",
      "macros": [
        { "pos": 0, "name": "Build", "icon": "hammer", "color": "#E0A030",
          "mode": "play_once",
          "actions": [ { "type": "key", "mods": ["CTRL","SHIFT"], "key": "B" } ] },
        { "pos": 1, "name": "Sign-off", "icon": "text", "color": "#3080E0",
          "mode": "play_once",
          "actions": [ { "type": "text", "value": "- Eitri, FORGE Master\n" } ] },
        { "pos": 2, "name": "Shot+paste", "icon": "camera", "color": "#30C060",
          "mode": "play_once",
          "actions": [
            { "type": "key", "mods": ["WIN","SHIFT"], "key": "S" },
            { "type": "delay", "ms": 800 },
            { "type": "key", "mods": ["CTRL"], "key": "V" }
          ] }
      ]
    }
  ]
}
```

### `pos` — now an identifier, not a slot

In v2, `pos` was a physical grid index 0-15 that mapped to a NeoTrellis key. In v3 it is:

- **a stable identifier** for a macro within its profile (what the app edits against), and
- **the ring ordering key** — wedges are laid out in ascending `pos`.

It is no longer capped at 15 and no longer implies a physical position. If the optional key pad
ever returns, `pos` 0-15 maps to keys exactly as before — which is precisely why it stays rather
than being replaced by an array index.

**`version` is 3** as of M8b (2026-08-26). The schema is a strict relaxation — every v2 file is a
valid v3 file — so v3 firmware reads v2 fine, and a document with no `version` at all is treated
as legacy rather than as an error.

**The bump is backed by an actual check.** Both firmwares now refuse a document declaring a version
*above* what they understand, rather than loading it and silently dropping whatever they cannot
interpret. On load a refused file falls back to the built-in defaults; on BLE save it is rejected
before anything reaches flash, and the refusal is reported to the app.

> Correcting this section's earlier rationale, which claimed the bump existed so "v2 firmware
> refuses a v3 file": it never did. No firmware read the field — the only occurrences in the tree
> were the two default-JSON literals — so a v2 build handed a v3 file loaded it and dropped the
> macros, which is exactly what the bump was supposed to prevent. That cannot be fixed
> retroactively in already-deployed v2 builds. What the check buys is forward-looking: from v3
> onward, a version a device does not understand is refused rather than mangled.

### Limits

| Limit | Value | What it governs |
|---|---|---|
| `MAX_MACROS` | **32** | Highest legal `pos` is 31. A data-model ceiling. |
| `RUNNING_SLOTS` | **16** | Macros playing simultaneously. A RAM limit. |

These were one number only because a single array served both jobs — `runningMacros[]` was indexed
*by* `pos`, which is what actually enforced the cap. It is now a pool keyed by macro identity, so
the two limits are independent.

32 is headroom, not a usable ring size: at 32 macros a wedge spans 11° and the 45×45 px icons stop
fitting somewhere around 16–20. Legibility binds long before the cap does.

A fire arriving when all 16 running slots are busy is **refused and logged**, never allowed to evict
a running macro — eviction can strand held modifiers or mouse buttons, since the stop-all path is
the only one that releases HID state.

### Accepted divergence: the M5Dial ring

On the **M5Dial**, a macro above `pos` 15 stores, loads, fires and stops correctly, and is
triggerable from the Companion App and over BLE — but **does not appear on that board's ring**,
which is 16 fixed dots (`angle = -PI/2 + (i * PI*2/16.0)`) rather than the Waveshare's
N-wedges-sized-to-fill.

This is deliberate. M8b uncapped that board's *engine*, not its ring. Closing the gap means porting
the dynamic wedge ring onto M5GFX at 240×240 without LVGL — most of the deferred M5Dial catch-up,
with its own hardware rounds — and belongs with the M5Dial security gate and its non-atomic write,
not here. It is the one place the two supported boards genuinely differ in what the user can reach.

### `settings.orientation` — dial rotation (shipped in M9)

An int `0..3`: `0` = 0°, `1` = 90° CW, `2` = 180°, `3` = 270° CCW. The Companion App's Settings
dialog reads and writes it (`dashboard_screen.dart:388-402`, `draupnir_state.dart:481-492`), and
**both firmwares now consume it.** The **M5Dial** always did — `M5_M6_config.ino` calls
`M5Dial.Display.setRotation(orientation)` on every `save_profiles`, and M5GFX rotates touch along
with the display in that one call. The **Waveshare** has no equivalent single-call API, and its
ignoring the field was a port gap rather than an unbuilt feature: the app wrote the setting, one
supported board honoured it, the other silently did not. M9 closed that.

**Mechanism.** Rotation happens in the SH8601 panel via MADCTL (register `0x36`), not in software.
`lcd_bsp.c` previously shipped a compile-time 90° path; M9 made it runtime-selectable from
`settings.orientation` across all four cases, MADCTL paired with a matching touch transform in
`example_lvgl_touch_cb()`. **Not** `esp_lcd`'s rotation API or LVGL's `sw_rotate`:
`panel_sh8601_swap_xy()` is `ESP_ERR_NOT_SUPPORTED` unconditionally and `mirror_y` is unsupported
too (only `mirror_x` works), and software rotation would re-rotate every flush on a display already
doing ten stripe-flushes per frame. The panel is square (360x360), so no dimension swap is needed
anywhere. MADCTL values are the standard `0x00`/`0x60`/`0xC0`/`0xA0`, and **all four orientations
are now proven on hardware**, display and touch together.

> **The trap, and it cost a hardware round: the SH8601 is driven over QSPI, so a command must carry
> the write opcode** — `(cmd & 0xff) << 8 | (0x02 << 24)`. Sent as a raw byte, MADCTL is silently
> ignored: no error, no effect. This presented as "touch rotates but the display does not", because
> the touch transform is plain C and worked regardless of the panel.

**Storage: `profiles.json`, not NVS — unlike brightness, deliberately.** Brightness has one writer
(the device), so NVS is authoritative and JSON is seed-only. Orientation has two writers (the app,
and eventually an on-device Settings item), which breaks that pattern either way — the only fix is
the device writing its own changes back to `profiles.json` via the H4 atomic-write path, making a
second NVS copy redundant. It also keeps the app's dropdown from showing a stale value after an
on-device change. Applies live on profile save (cheap at runtime; hooked into the existing reload
path under `lvgl_lock()`). The open worry there — whether changing MADCTL mid-partial-refresh leaves
flush regions correct — did not materialise: rotating from the app repaints cleanly, and no torn or
offset stripes were seen. That is an observation from ordinary use, not a targeted stress test, so
boot-only application remains the fallback if it ever does surface. Encoder direction does not
change with orientation — only the display and touch transform rotate.

### Action types

| type | fields | notes |
|---|---|---|
| `key` | `mods[]`, `key` | modifier combo + key; `key` may be a character or a named special (`F5`, `ENTER`, `HOME`, …) |
| `text` | `value` | typed string (US layout for v1) |
| `delay` | `ms` | pause between steps |
| `consumer` | `code` | `VOL_UP` / `VOL_DOWN` / `MUTE` / `PLAY_PAUSE` / `NEXT` / `PREV` |
| `mouse` | `button`, `event` | `button`: `LEFT`/`RIGHT`/`MIDDLE`/`MB4`/`MB5`, or `SCROLL_UP`/`SCROLL_DOWN`/`SCROLL_LEFT`/`SCROLL_RIGHT`. `event`: `CLICK` (default) / `PRESS` / `RELEASE` / `DOUBLE_CLICK` |

> v2 listed `mouse_move` (relative `dx`/`dy`) and `mouse_click` as separate types. The
> implementation converged on a single `mouse` type and **relative movement was never built**.
> v3 documents what exists. Relative move is parked in §12 — add it as
> `{ "type": "mouse", "event": "MOVE", "dx": …, "dy": … }` if it is ever wanted.

### Macro `mode`

| mode | behavior |
|---|---|
| `play_once` | run the action list once (default) |
| `toggle` | **repeat the action list continuously** until the macro is fired again |

`toggle` is a repeat-loop, not a stateful on/off. It suits "jiggle the mouse" or "spam a key",
and is actively wrong for one-shot toggles like Caps Lock — the OS already latches those.

### Icons

`icon` names a Feather icon. The Companion App additionally renders it to `icon_xbm`, an 18x18
1-bpp bitmap as a 108-char hex string. Two standing rules:

- **The device must actually render icons** (success criterion #3). Shipping `icon_xbm` to a
  device that ignores it is pure payload weight on a size-constrained transport.
- **`icon_xbm` is derived data.** If ring wedges move to a vector/font glyph, drop it from the
  wire format rather than sending both.

---

## 7. Config: BLE Companion App

The Flutter app is the **only** configuration surface. There is no web UI and none is planned.

### Transport
Nordic-UART-style GATT service: RX characteristic (app -> device, write) and TX characteristic
(device -> app, notify). Payloads are newline-terminated JSON, chunked in both directions.
Because `notify()` has no delivery guarantee, each device->app chunk carries a 1-byte sequence
number and the app echoes a 2-byte `[0xFE, seq]` ack; the device resends on timeout.

Commands: `get_profiles`, `save_profiles`, `trigger`.

`get_profiles` takes one optional flag, `"include_icons": true`. Absent or false — the common path
— strips every `,"icon_xbm":"<hex>"` pair from the response on the fly, because the app needs only
the icon *name*; the 18×18 1bpp bitmap is display-side data it never renders. True passes them
through, and exists so that **export** can put the bitmaps in a file. Without it a shared or
transferred profile silently loses every custom icon.

The two boards implement the flag differently, and this is worth knowing before editing either:
the Waveshare *composes* `IconXbmFilterSink` around `BleChunkSink`, so the icon path **bypasses**
the filter (and correspondingly performs only the sink's flush — the filtered path needs both, and
getting that wrong truncates the response tail into what looks like malformed JSON); the M5Dial
*folds* the stripping into `BleChunkSink` itself, so its sink takes a constructor flag instead.

### Advertised names *(2026-08-29)*
Each board advertises a distinct name — Waveshare = **`Draupnir`**, M5Dial = **`Draupnir_Mini`** —
because both were previously `Draupnir` and the app connected to whichever answered the scan
first, with no way to tell them apart or to choose. The app still **matches loosely** (any name
containing `draupnir`, case-insensitive, or the NUS service UUID), so the name is a label for
humans, not a protocol constant: a new board picks a new name without an app change. When the scan
finds more than one, the app asks which to connect to; with one, it connects straight through.

### Config Mode *(M5Dial only — no longer a gate, as of 2026-09-06)*
The M5Dial used to serve **no config command outside `CONFIG_MODE`**, answering everything else
with `{"status":"error","message":"Not in Config Mode"}`. That check is **removed**: it was a
physical-presence gate, not authentication, and it stopped nothing once the user swiped down. Both
boards now gate on the encrypted, authenticated link instead.

`CONFIG_MODE` survives as an **informational screen** — swipe down to see the board's BLE name and
whether the app is attached. It authorizes nothing. The app keeps its "Config Mode required" panel
as a **fallback for an M5Dial still running pre-gate firmware**, where the message is still
correct.

### Security — standard BLE pairing
- **Pairing:** the device displays a passkey on screen (IO capability = DisplayOnly); the user
  enters it in the phone's pairing dialog. Bonding is stored so subsequent connects are silent.
- **Enforcement:** the config characteristics **require an encrypted, authenticated link**. This
  must be enforced by GATT permission flags on the characteristics themselves, not merely
  requested at connect time — a central that ignores a security *request* must still be unable to
  read or write.
- **No application-layer token.** The `token` field, the `pair` command, and the `pairingToken`
  NVS entry are all removed from firmware, app, and schema.

Why this matters more than it sounds: the device is a **keyboard**. An unauthenticated write path
is arbitrary keystroke injection into the attached host, plus profile exfiltration.

**Enforced on both boards, as of 2026-09-06.** Each sets `ESP_LE_AUTH_REQ_SC_MITM_BOND` with
`ESP_IO_CAP_OUT`, displays a per-connection passkey, and carries `_ENC`/`_AUTHEN` permission flags
on the RX characteristic plus `_ENC` on TX. The M5Dial additionally refuses, at the command
handler, anything arriving on a link whose `sec_state` is not `encrypted && authenticated`, and
logs that state on every accepted command — a backstop in case the permission flags ever silently
stop enforcing, which is this API's known failure mode.

**Proven by refusal, not just by acceptance, on both boards.** Each gate was tested with a hostile
central that declined pairing and then attempted to write: the Waveshare on 2026-08-07, the M5Dial
on 2026-09-07. In both cases the command handler never saw the bytes. This is the criterion that
matters — a gate that accepts authorized traffic proves nothing about what it refuses — and it is
why the M5Dial's claim was held as explicitly *unverified* for a day after its positive-path round
passed, rather than being folded in.

Belt-and-braces behind the flags, on the M5Dial: a `#error` plus two `static_assert`s make a
silently-zero permission flag a compile error (the assert was confirmed to actually fire by
inverting it), and the command handler independently refuses any link whose `sec_state` is not
`encrypted && authenticated`, logging that state on every accepted command.

Going from no security to bonding is a **breaking change** for existing M5Dial users: the old
device entry must be removed from the phone's Bluetooth settings before the app will work again.
Observed on hardware — after removing the stale entry, pairing proceeds normally.

### Editing flow
Edit profiles and macros; assign name, color, icon, mode, and action sequence; reorder; set
brightness. Save -> `save_profiles` -> validate -> write `profiles.json` -> reload -> re-legend
the ring live, without a reboot.

---

## 8. Storage, robustness & power

- **Macros/profiles:** `profiles.json` in LittleFS.
- **State:** last profile + brightness in NVS (`Preferences`), **written on change and restored on
  boot.** Reading it without ever writing it is the same as not having it.
- **Writes must be atomic:** serialize to `/profiles.json.tmp`, verify, then rename. A power loss
  mid-save must never leave an unparseable config.
- **Parse failure must be recoverable:** if `profiles.json` is missing *or unparseable*, fall back
  to the built-in default and log it. Only handling the missing-file case leaves corruption
  unrecoverable without a reflash.
- **Config transfers are bounded:** the RX reassembly buffer must be sized for a realistic
  multi-profile document, and an overflow must reset cleanly and report an error — never wedge the
  command channel until reboot.
- **Power:** USB-C. Without the key pad the LED current budget that dominated v2 is gone; screen
  brightness is now the main draw.

---

## 9. Dev environment

- **Arduino IDE or arduino-cli**, ESP32 Arduino core (S3). PowerShell on Windows; use `.ps1`
  scripts for anything nontrivial.
- Set **USB-OTG / "USB CDC On Boot"** so HID and flashing coexist.
- M5Dial FQBN in use:
  `m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB`
- M5Dial flashing: hold G0, plug USB, release.
- See `docs/M0_Setup_and_BringUp.md` and `docs/Toolchain_arduino-cli.md`.

---

## 10. Milestones & current state

Honest status, not aspiration.

| # | Milestone | State |
|---|---|---|
| M0 | Board bring-up (screen, encoder, touch) | **Done**, both boards |
| M1 | USB HID hello | **Done** |
| M2 | Macro engine: combos, text, consumer, mouse, delays | **Done** |
| M3 | Ring UI: dynamic wedges, colors, selection, tap-to-fire | **Done** (Waveshare) |
| M4 | Profile store: LittleFS + defaults | **Done** |
| M5 | BLE transport: chunked send/ack, Companion App connects | **Done** |
| M6 | **Config hardening** — pairing enforcement, atomic writes, RX bounds, reload safety | **Done (2026-08-07)** — see note below |
| M7 | **Persistence** — write `activeProfile` + brightness to NVS and honor them at boot | **Done (2026-08-08)**, verified on hardware |
| M8 | **On-device profile switching** with directional indicators | **Done (2026-08-13)**, verified on hardware — grew beyond its original scope, see note below |
| M8b | **Uncap `pos`** — key running-macro state by identity, not slot; bump the default to `version: 3` and actually check it | **Done (2026-08-26)**, verified on hardware 2026-08-29 — both boards, full criteria list |
| M9 | **Icons on the ring** + dial orientation + rotary macro mode — encoder detent alignment dropped, see note below | **Done (2026-08-22)**, verified on hardware — Waveshare 2026-08-22, M5Dial icon-merge 2026-08-25 |
| M5Dial security gate | **Close the second board's config channel** — BLE pairing/bonding + GATT permission flags, delete Wi-Fi and the LAN-reachable web API, atomic profile write, passkey screen | **Done (2026-09-06)**, verified on hardware — positive path 2026-09-06, hostile-central negative test 2026-09-07 (§7) |
| M10 (export/import) | **Profile export/import** — `include_icons`, an enveloped JSON file, whole-config restore and single-profile share, pre-import snapshot with undo | **Done (2026-09-08)**, verified on hardware — M5Dial only; the cross-device transfer is **not** yet verified (§4 of HANDOFF) |
| M10 (haptics) | Buzzer/haptic feedback | **Tabled** — check whether a DRV2605 is on the Waveshare bus at all before debugging the CST816 conflict; see `docs/HANDOFF.md` §6 |
| M11 (UI/UX polish) | **Pre-publish polish** — M5Dial PCNT encoder, Waveshare swipe-vs-tap, hub compass caret, save-path feedback, icon picker defects, macro icon vocabulary, colour palettes, text-label fallback | **Done (2026-09-23)**, verified on hardware — both boards; PR #16 |
| M12 (icon rendering) | **Crisp icons on the Waveshare** — curated glyph set compiled in and drawn from the `icon` name, with an app-supplied higher-resolution bitmap covering names the firmware does not know | **Next** — design in progress |
| M13 (OTA) | **Firmware update from the Companion App** — check for, transfer and apply a firmware image over BLE, with rollback | **Planned** — see note below |

**M13 (OTA)** is a prerequisite for the product being maintainable in the field, and it is also
what makes M12's design affordable. M12 deliberately compiles its glyph set into the firmware
rather than loading a font pushed over BLE — a loader would mean a filesystem driver in LVGL, a
resumable bulk transfer, PSRAM enabled (currently disabled on purpose, §3), and an
attacker-supplied binary parsed inside the render loop, immediately before a public release. The
hybrid avoids all of that by letting the app supply a bitmap for any glyph the firmware lacks, so
new icons work everywhere immediately and a firmware release merely *promotes* them to crisp.
That trade only stays comfortable if shipping firmware is routine, which is what M13 buys.

The board is already provisioned: the `default_8MB` partition table carries `app0` and `app1` OTA
slots of 0x330000 each, so no layout change is needed. Open questions are transfer rate over the
existing 100-byte chunked transport (a ~1 MB image is ~10,000 chunks), resumability across a
disconnect, signature verification, and the rollback trigger — `esp_ota_mark_app_valid_cancel_
rollback()` exists for this but needs a health check to gate it.

**M6:** done-criterion was the H1 negative test, which passed on hardware 2026-08-07 — an
unbonded central wrote to the RX characteristic and the command handler never received the bytes,
refused at the GATT layer. Two items from the original hardening test plan were never run and
remain open, carried forward rather than dropped: H2's overflow-recovery test and H4's
corruption-recovery test. Heap across a save/reload cycle was also never proven flat over a long
soak — the one cycle observed went 61,720 → 61,392 bytes, consistent with the document growing
(919 B) rather than a leak, but a single cycle can't tell the two apart.

**M8:** what shipped is well beyond the original "profile switching with directional indicators"
line. Three hardware feedback rounds turned it into a full ring rework — a rotating ring under a
static 12 o'clock selector, a centre label stack (macro name large, profile name small), a tinted
selection bloom, and real FontAwesome chevron indicators — confirmed on hardware with the owner's
own words: "This looks great."

**M9:** done-criterion was the owner's hardware sign-off on the Waveshare, given 2026-08-22 after
three feedback rounds ("This looks good, I think we can mark M9 complete."). What shipped differs
from the original "icons + encoder detent alignment" line in three ways, each worth recording
rather than silently rewriting the milestone description:

- **Dial orientation was added.** This turned out to be a port gap rather than new product surface:
  the Companion App's dropdown had always written `settings.orientation` to `profiles.json`, and the
  M5Dial firmware had always consumed it via `M5Dial.Display.setRotation()`, but the Waveshare
  ignored the field entirely. M9 closed that gap for all four values (`0`/`1`/`2`/`3`), in both the
  MADCTL display write and the touch-coordinate transform — see §6 for the schema and mechanism.
- **Rotary macro mode was added.** Same class of gap, and the more dangerous one: the app already
  offered it and the M5Dial already implemented it, while the Waveshare silently ran a
  rotary-mode macro as an ordinary `play_once`, discarding the distinction without any error. M9
  brought the Waveshare to parity, including matching the M5Dial's behavior of *not* stopping macros
  on rotary entry while stopping only the rotary binding (not all macros) on exit.
- **Encoder detent alignment was dropped.** It was superseded by the M7/M8 ring rework: selection
  now always lands centred at 12 o'clock regardless of which detent it came from, so there was
  nothing left to align. The remaining ~15% mechanical double-step (two contact closures per
  detent) was measured, traced to the physical switch rather than firmware, and deliberately
  accepted rather than fixed with a lockout window that would also cap deliberate fast turning.

**Verified on both boards.** The Waveshare half was signed off 2026-08-22. The M5Dial half —
porting the icon-merge fix that closes a live data-loss bug (editing one macro was wiping every
other macro's icon) — was flashed and its acceptance criterion run on **2026-08-25**: editing one
macro no longer wipes the icons on the others. See `docs/HANDOFF.md` §4 for the full breakdown.

---

## 11. Tradeoffs

- **Ring vs. keys.** A ring wedge is a bigger target than expected and needs no second puck, but
  it degrades as macro count climbs — wedges get thin and icons stop being legible. Practical
  ceiling is roughly 10-12 per profile; beyond that, use more profiles. The app should nudge
  toward splitting rather than silently rendering slivers.
- **No physical keys** means no eyes-free muscle memory. The knob's detents partly compensate
  (count clicks from a known position); this is the main thing to re-evaluate in long-term use.
- **BLE-only config** means no desktop config path. Accepted: the phone app is better, and a
  desktop client can speak the same BLE protocol later if wanted.
- **Two boards** costs a real maintenance tax. It is only worth paying while the shared-core
  boundary in §3 holds.

---

## 12. Expansion path (optional)

- **16-key RGB pad** as a companion puck (NeoTrellis) or hotswap mechanical (4x NeoKey 1x4).
  `pos` 0-15 already maps to it.
- **Relative mouse movement** — `event: "MOVE"` with `dx`/`dy`.
- **Second encoder / extra keys** on a free GPIO port.
- **Desktop client** over the same BLE protocol.
- **Haptics** — see the SmartKnob note in Appendix A.

---

## 13. Risks & open questions

- **Encoder detent alignment.** Measured on the M5Dial: 4 encoder counts per physical detent, so
  the selection fought the detents. Screen feedback must match tactile feedback on both boards —
  verify the Waveshare `bidi_switch_knob` ratio explicitly rather than assuming 1:1.
- **Encoder push button.** Not wired in the Waveshare firmware. Confirm whether the hardware has
  one; if it does, wire it to "fire selection" so the center tap is not the only path.
- **Heap pressure.** LVGL draw buffers, a JSON document, BLE, and USB all resident at once.
  Hard on the **M5Dial**, which genuinely has no PSRAM. On the **Waveshare** board 8 MB of PSRAM is
  available and simply not enabled yet — so budget config payloads and any icon cache carefully on
  the Dial, and treat Waveshare heap limits as a setting to revisit rather than a ceiling.
- **Blocking in loop().** BLE chunk ack retries and per-character HID pacing both block the loop
  task, stalling in-flight macros. Needs a bounded time budget per tick.
- **Keyboard layout.** HID sends keycodes; typed `text` assumes US layout for v1.
- **Two-board drift.** The first change that has to be written twice means the §3 boundary leaked.

---

## 14. Shopping list

- **Waveshare ESP32-S3 round-knob board** (1.8" 360x360 AMOLED). *(have)*
- **M5Stack Dial v1.1** — second target. *(have)*
- USB-C data cable.
- *(Optional, later)* NeoTrellis 3954 + 4x4 silicone pad + acrylic case + Grove-to-STEMMA cable,
  or 4x NeoKey 1x4, for the key-pad expansion (§12).

---

## Appendix A — Inspiration & parking lot

*Not committed scope. Revisit at P2/P3.*

**Form factor (decide at P2).** With keys demoted, the leading silhouette is a **standalone knob
puck** — screen in the cap, encoder body below, weighted base. The v2 candidates (radial key ring;
Megalodon-style asymmetric cluster) survive only as key-pad-expansion shapes.

**Smart-knob upgrade.** *Pragmatic:* round LCD + normal encoder (what P1 already is). *Halo:* a
haptic SmartKnob (Scott Bezek's open Apache-2.0 SmartKnob View — BLDC + magnetic encoder +
strain-gauge press) giving software-defined detents that change feel per context. Now a much more
natural fit than it was in v2, since the knob *is* the product. Big engineering jump (FOC motor
control, current, bulk). Seeed sells a DevKit to prototype without building the motor assembly.

**Per-key LCD deck.** Filed as a separate, more ambitious product (screenless hub, every key its
own LCD). SPI-per-key, RAM/bandwidth, and ~$145/16 keys put it out of scope; it also earns its
cost only when there is *no* central screen, which is the opposite of this device.

### Reference links
- SmartKnob (scottbez1/smartknob, Apache-2.0); SmartKnob DevKit (SeedLabs)
- DOIO KB16 Megalodon (QMK/VIA reference for the key-pad expansion)
- Adafruit NeoTrellis 3954 / NeoKey 1x4 (key-pad expansion paths)

---

## Appendix B — The v2 two-puck design (historical)

Preserved because it is the origin of the `pos` model and the path back if keys return.

P1 was **M5Dial + Adafruit NeoTrellis 4x4**, cabled PORT.A (Grove I2C, G13/G15) to STEMMA JST-PH
via an adapter, seesaw at I2C 0x2E, 5V over Grove for the NeoPixels. The unifying idea was
**dial detent N = key N = color N = icon N**: 16 detents mapped 1:1 to 16 elastomer keys, each lit
in its macro's color as a persistent legend.

Configuration was a browser over the Dial's own Wi-Fi (SoftAP + `ESPAsyncWebServer`), later
augmented by the Companion App with a token-based pairing scheme. **Both the web UI and the token
scheme are removed in v3** — the app replaced the former, BLE bonding replaced the latter.

Known constraints from that era that still inform v3: NeoPixel current over a thin Grove cable
(cap `ledBrightness`), the Grove-vs-STEMMA connector mismatch, and the NeoTrellis PCB shipping
without buttons.
