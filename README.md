# Draupnir

**A self-contained USB-HID macro controller in a knob: round touch screen + rotary encoder, configured from your phone.**

Draupnir shows your macros as a ring of colored wedges — one wedge per macro, sized to fill the
ring. Turn the knob to select, tap the center to fire, or tap a wedge directly. It enumerates as a
standard USB keyboard/mouse/media device, so there is **no host software and no drivers**. Macros
live in the device's own flash; you edit them over Bluetooth from the Companion App.

> **Status: work in progress (P1 / proof of concept).** USB HID, the macro engine, the ring UI, the
> profile store, and BLE config all run on hardware today. Config-path hardening, on-device profile
> switching, icon rendering, and settings persistence are still open — see
> [the spec](docs/Draupnir_Spec.md) §10 for honest per-milestone status. **Not yet suitable for
> leaving plugged into a machine you care about**: the BLE config characteristics do not yet
> enforce pairing.

## Hardware

One puck. No key pad, no second device, no cable between components.

| | Primary target | Second target |
|---|---|---|
| Board | Waveshare ESP32-S3 knob | M5Stack Dial v1.1 |
| Screen | 1.8" round AMOLED, 360x360 (SH8601, QSPI) | 1.28" round IPS, 240x240 (GC9A01) |
| Touch | CST816 | FT3267 |
| Input | Rotary encoder | Rotary encoder + knob button |

Both boards run the same macro engine, the same `profiles.json` schema, and the same Companion
App — only the display/input layer differs.

A 16-key RGB pad (Adafruit NeoTrellis or NeoKey) is an **optional future expansion**, not part of
the base device. Earlier versions of this project made it central; field testing showed the knob
carries the interaction on its own.

## Usage

1. **Plug in.** Connect the board to your computer with a data-capable USB-C cable. It enumerates
   immediately as a USB Keyboard/Mouse/Consumer device.
2. **Fire macros.**
   - Turn the knob to move the selection around the ring; the center names the selected macro.
   - Tap the center to fire the selection.
   - Tap any wedge to select and fire it directly.
3. **Configure.** Open the Draupnir Companion App and connect over Bluetooth. The first time, the
   device displays a passkey on screen — enter it in your phone's pairing dialog. That is standard
   BLE pairing; there is no separate in-app pairing step and no account.
4. **Edit macros.** In the app: add/rename/delete profiles, set a macro's name, color, icon, and
   mode, and build its action sequence — keystrokes, text strings, media/system keys, mouse
   clicks and scrolling, and delays. Save writes to the device and the ring re-legends live.

There is **no web UI and none is planned.** The Companion App is the only configuration surface.

## Macros

Each macro is an ordered list of actions:

| Action | What it does |
|---|---|
| `key` | A key with modifiers — `Ctrl+Shift+B`, `F5`, `Enter`, … |
| `text` | Types a string (US layout) |
| `consumer` | Media/system keys — volume, mute, play/pause, next, previous |
| `mouse` | Click, double-click, press/release, and scrolling |
| `delay` | Pause between steps, in milliseconds |

Macros run in one of two modes: `play_once` (default) or `toggle`, which **repeats the sequence
continuously** until the macro is fired again.

## Building the firmware

See the docs folder:
- [Toolchain (arduino-cli)](docs/Toolchain_arduino-cli.md) — fast, headless build + flash.
- [M0 Setup and Bring-Up](docs/M0_Setup_and_BringUp.md) — step-by-step for the Arduino IDE.

Firmware trees live under `firmware/`; `Waveshare_LVGL_Test/` is the primary one.
Requires the ESP32 Arduino core (S3) with **USB CDC On Boot** enabled, plus LVGL and ArduinoJson
(and M5Unified for the M5Dial target).

The Flutter Companion App is in `companion_app/`.

## Documentation

- [**Full spec**](docs/Draupnir_Spec.md) — concept, hardware, data model, BLE protocol, milestones.
- [P1 Polish Spec](docs/P1_Polish_Spec.md) — open UI/UX findings from field testing.
- [BLE Profile Fetch Debugging](docs/BLE_Profile_Fetch_Debugging.md) — the chunked-transport war story.

## License

MIT. Firmware and hardware are open — see [LICENSE](LICENSE).
