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
| Controller | ESP32-S3, native USB (OTG/CDC) |
| Display | 1.8" round AMOLED, **360x360**, **SH8601** over **QSPI**, 16 bpp |
| LCD pins | CS 14, PCLK 13, D0-D3 15/16/17/18, RST 21, backlight 47 (LEDC PWM) |
| Touch | **CST816**, I2C addr **0x15**, SDA 11 / SCL 12 |
| Encoder | Rotary, A = **GPIO 8**, B = **GPIO 7** |
| Encoder button | **Not wired in firmware** — `knob_config_t` exposes A/B only. Confirm whether the hardware has a push action before relying on it. |
| Memory | LVGL draw buffers are allocated `MALLOC_CAP_DMA` from internal RAM; **no PSRAM is used**. Treat heap as tight. |
| UI stack | LVGL + `esp_lcd_sh8601` |

### Second target — M5Stack Dial v1.1

| Item | Detail |
|---|---|
| Controller | M5StampS3 — ESP32-S3FN8, native USB |
| Flash | 8 MB (**no PSRAM, no SD**) |
| Display | 1.28" round IPS, GC9A01, 240x240, FT3267 touch |
| Input | Rotary encoder (16 detents / 64 PPR) + knob button |
| Extras | Buzzer, RTC; PORT.A (Grove I2C, G13/G15), PORT.B (GPIO, G2/G1) |
| Download mode | Hold **G0** on the back Stamp, plug USB-C, release |

### Supporting two boards without forking the product

Both targets must share the schema, the BLE protocol, the macro engine, and the Companion App.
Only the display/input/driver layer differs. Concretely, the boundary is:

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
  "settings": { "brightness": 160, "buzzer": true },
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

**`version` bumps to 3.** The schema is a strict relaxation, so v3 firmware reads v2 files fine.
The bump exists so *v2 firmware refuses a v3 file* rather than silently dropping every macro with
`pos > 15`.

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
| M6 | **Config hardening** — pairing enforcement, atomic writes, RX bounds, reload safety | **Open — blocking** |
| M7 | **Persistence** — write `activeProfile` + brightness to NVS and honor them at boot | **Open** |
| M8 | **On-device profile switching** with directional indicators | **Open** |
| M9 | **Icons on the ring** + encoder detent alignment | **Open** |
| M10 | Polish — buzzer/haptic feedback, brightness UI, export/import | **Open** |

M6 is sequenced first because it is the difference between a prototype and a device that is safe
to leave plugged into a machine.

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
- **Heap pressure.** No PSRAM on either board, with LVGL draw buffers, a JSON document, BLE, and
  USB all resident. Config payloads and any icon cache must be budgeted, not assumed.
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
