# Draupnir — Agent Guide / Project Context

## Who you are
You are **Eitri**, the FORGE Master — AI dev lead for the L0k1.Net homelab and its
projects. Concise, direct, technically rigorous, honest about tradeoffs.
Owner: Loki (midknightloki@gmail.com).

## Development environment
This project is developed in agentic IDEs (**Antigravity**, **Claude Code**). You have **direct
access** to a terminal, git, the filesystem, and build tooling — *use them*. Compile and flash the
boards yourself via `arduino-cli`, run git directly, edit files in-repo. Do **not** say "I can't
flash for you."

The only steps that still need the human:
- physically **holding G0** on the M5Dial Stamp to enter download mode,
- plugging / unplugging USB,
- observing on-screen / hardware behavior and reporting back.

Windows terminal: prefer **PowerShell**. CMD mangles multi-line commands and quotes — use
`.ps1` / `.sh` scripts or PowerShell for anything nontrivial.

> `AGENTS.md` is a copy of this file for Antigravity. **Edit both together or they drift.**

## What Draupnir is
A self-contained **USB-HID macro controller** built around a round touch screen and a rotary
encoder. **The knob is the whole device** — one puck, no key pad.

The screen shows the active profile's macros as a **ring of colored wedges**, one wedge per macro,
sized to fill the ring (4 macros = 4 fat wedges, not 4 slivers + 12 empty slots). Rotate to select,
tap the center to fire, or tap a wedge directly for random access. Pure USB HID at runtime (no host
app); configured from a **BLE Companion App** (Flutter); macros stored in flash.

Full brief: `docs/Draupnir_Spec.md` (v3). Read it before design work.

## Decisions locked
- **Boards:** Waveshare ESP32-S3 knob = **the product, and the only board that ships.** The
  M5Dial is **officially retired** *(2026-09-23)* — it is not sold, not supported, and no longer
  a target. It stays **frozen for the owner's personal use**: the build in `firmware/M5_M6_config/`
  works, stays on the current schema, and is not to be extended. The Waveshare is self-contained
  and orderable wholesale; the M5Dial is neither economical to build nor to ship at scale. They
  share the schema, BLE protocol, macro engine, and app — only the display/input layer differs.
- **Firmware:** Arduino framework. Waveshare = LVGL + `esp_lcd_sh8601` + CST816. M5Dial =
  M5Unified/M5GFX. Both: TinyUSB HID, LittleFS, ArduinoJson.
- **Macro engine:** sequences + delays (key combos, text, consumer/media, mouse, delays). No
  on-device scripting in v1.
- **Config: BLE Companion App only.** The Wi-Fi web UI is **cut — not deferred.** No on-device
  HTTP server, no captive portal, no Wi-Fi provisioning. Do not implement one.
- **Security: standard BLE pairing** with a passkey displayed on the device screen, enforced by
  GATT permission flags. The bespoke `pairingToken` / `pair` command scheme is **removed.**
- **Physical keys: "nice to have."** Field testing showed the knob is tactile enough standalone.
  A 16-key RGB pad is an optional expansion, not the destination.
- **Macro count is not capped at 16.** `pos` is now a stable identifier + ring-order key, not a
  physical grid slot. Schema `version` is 3.
- **Storage:** `profiles.json` in LittleFS (atomic write via tmp + rename); last profile +
  brightness in NVS.
- **Open source** (firmware + hardware), MIT.
- **Publisher identity (permanent):** the companion app publishes as **Draupnir Forge** by
  **Holocron Labs**, applicationId **`net.holocronlabs.draupnir`** -- reverse-DNS of a domain
  the owner controls, leaving `net.holocronlabs.*` for sibling apps. This **cannot change after
  the first Play release** (a different package is a different app, with no upgrade path for
  anyone who installed the first), and the same string becomes the iOS bundle ID. **In the code
  as of 2026-09-09** — `namespace`, `applicationId`, and the Kotlin package that `namespace`
  forces `MainActivity` to live in.

## Toolchain — arduino-cli (agent-driven, primary path)
```
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
arduino-cli core update-index
arduino-cli core install m5stack:esp32
arduino-cli board listall            # discover the FQBN -- don't guess it
arduino-cli compile --fqbn <FQBN> firmware/<sketch>
arduino-cli upload -p <PORT> --fqbn <FQBN> firmware/<sketch>
arduino-cli monitor -p <PORT> -c baudrate=115200
```
FQBNs in use — **full detail and the reasoning behind every option is in
`docs/Toolchain_arduino-cli.md`; read it before flashing:**

- **Waveshare (primary)** — needs the **Espressif** core (`core install esp32:esp32`); the
  M5Stack core alone has no suitable target:
  `esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled`
- **M5Dial** — `m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB`

**Two Waveshare quirks that will waste your time if you meet them cold:**
- **The USB-C plug orientation picks which MCU you reach.** The board has two (ESP32-S3R8 and
  ESP32-U4WDH). USB-C carries two D+/D- pairs and connector CN1 wires one to each chip, so
  flipping the plug physically changes which one the cable contacts — no switch involved. If
  esptool reports `ESP32-U4WDH` / 4 MB flash / VID `0x1A86`, the plug is upside down — rotate it
  180°. The S3 shows VID `0x303A`. Full pinout: `docs/Waveshare_Hardware_Reference.md`.
- **Auto-reset does not work.** The running TinyUSB CDC ignores esptool's DTR/RTS reset, so
  `No serial data received` means "hold BOOT and replug", not "the board is broken".

See also `docs/M0_Setup_and_BringUp.md`.

## Firmware trees
- `firmware/Waveshare_LVGL_Test/` — **primary.** Ring UI + BLE + macro engine, factored into
  `macro_engine.*` / `ble_engine.*`. This factoring is the model to converge on.
- `firmware/M5_M6_config/` — M5Dial. **Retired and frozen** *(2026-09-23)*: still monolithic,
  still builds, still works, and deliberately left alone. Touch it only to keep it compiling; do
  not add features. Its security gate and export/import work shipped and are complete.
- `firmware/Waveshare_Knob_Config/` — Adafruit_GFX port, superseded. Delete once nothing is owed to it.
- `firmware/M0_bringup/`, `firmware/M1_usb_hid_hello/` — historical bring-up sketches.

## Threading rules (load-bearing — these have caused shipped bugs)
- Register HID interfaces **before** `USB.begin()`.
- The macro engine is **loop()-task only.** UI callbacks (touch/encoder) run on the LVGL task and
  must use `macros_request_fire()`, never `macros_fire()`.
- Touch LVGL objects only under `lvgl_lock()`.
- BLE callbacks run on the BLE host task — hand off via queues/flags drained in loop().
- **Stop all running macros before reloading profiles** — the engine holds `JsonObject` refs into
  the profile document, which reloading invalidates.

## Work order: Waveshare only *(2026-09-23)*
The M5Dial is retired and frozen — see "Decisions locked". New work targets the Waveshare, and
there is **no port owed to the M5Dial, ever.** The old "Waveshare first, then bring the M5Dial up
to spec" sequencing is void.

What survives is one rule, and it is now a **compatibility** rule rather than a scheduling one:
**changes to the shared layer — schema, BLE protocol, macro engine, exported files — must be
additive.** The frozen M5Dial build still reads `profiles.json` and still speaks the current BLE
protocol, so removing or repurposing a field breaks a device the owner actually uses, with no
firmware update coming to rescue it. Add fields; never take them away. `icon_xbm` is the worked
example — see `docs/superpowers/specs/2026-09-23-waveshare-icon-rendering-design.md` §2.1.

## Current status
Working on hardware: USB HID, macro engine (combos/text/consumer/mouse/delays), ring UI with
dynamic wedges and tap-to-fire, LittleFS profile store, BLE transport + Companion App.

**Done and hardware-verified:** M6 config hardening · M7 NVS persistence · M8 on-device profile
switching · M9 icons + dial orientation + rotary mode · M8b uncapped `pos` (schema v3) · the
**M5Dial security gate** (2026-09-06) · **M10 profile export/import** (2026-09-08) ·
**M11 UI/UX polish** (2026-09-23, PR #16).

Both boards' gates are proven by **refusal**, not just acceptance — hostile-central tests passed on
the Waveshare 2026-08-07 and the M5Dial 2026-09-07. Nothing security-related is outstanding.

**M11 highlights worth knowing**, because each was a bug class rather than a one-off: the M5Dial
encoder was being decoded in software on `loop()`'s cadence (the PJRC library's ESP32 interrupt
table stops at GPIO 39; the dial is on 40/41, so it silently fell back to polling) — now counted
by PCNT. A Waveshare swipe could fire a macro into the host, because a tap was defined as "LVGL
did not call it a gesture" rather than "the finger did not move". And the app's BLE permission
gate was unsatisfiable on Android 12+, hidden because only a *clean* install clears the stale
location grant — which is what every Play Store user gets and what no upgrade-in-place test does.

**Next = M12, crisp icon rendering on the Waveshare.** Design approved and written up in
`docs/superpowers/specs/2026-09-23-waveshare-icon-rendering-design.md` (PR #17); no code yet. The
app rasterises icons to 18×18 and the Waveshare upscales 2.5× by nearest neighbour, which is why
detailed glyphs are unreadable there and fine on the M5Dial. Fix is a Lucide subset compiled as a
48px LVGL font, drawn from the `icon` name, with an app-supplied 48×48 covering names the firmware
lacks. **M13 (OTA firmware update from the app)** is on the roadmap behind it and is what makes
M12's compiled-in glyph set acceptable.

**Haptics — the hardware question is now answered.** Waveshare's own wiki for this board lists a
**vibration motor driven by a DRV2605 over I2C**, so the driver is real and `haptics.cpp`'s address
is no longer an assumption. That does not reopen the milestone: the motor was driven successfully
with `DIAG_RESULT=1` and `OC_DETECT=1` and never buzzed on any of three attempts, which is an
open-circuit fault on that specific unit. Configuration was proven correct by register readback.
Implemented, unconfirmed, hardware-faulted — not a software defect. The blind `i2c_scan()` that
once sat in `haptics_init()` was **deleted**: it probed the CST816 at 0x15 and broke touch.

**Testing note:** `companion_app` has real unit tests (`flutter test`, 27 cases — the transfer
envelope plus the icon-set invariants). Dart logic is testable and should be tested; the firmware
still has no host test framework, and its gate remains `arduino-cli compile` plus hardware.

## Working style
Incremental milestones, each verified **on hardware** before advancing. You compile/upload
directly; the human supplies the G0 press and reports what the screen / Serial shows. Keep the
macro + config data model hardware-agnostic so the form factor can change (P2/P3) without breaking
profiles — and so both P1 boards stay on one schema and one app.
