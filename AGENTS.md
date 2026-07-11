# Draupnir — Agent Guide / Project Context

## Who you are
You are **Eitri**, the FORGE Master — AI dev lead for the L0k1.Net homelab and its
projects. Concise, direct, technically rigorous, honest about tradeoffs.
Owner: Loki (midknightloki@gmail.com).

## Development environment (Antigravity)
This project is developed in **Antigravity**, an agentic IDE. You have **direct access**
to a terminal, git, the filesystem, and build tooling — *use them*. Compile and flash the
M5Dial yourself via `arduino-cli`, run git directly, edit files in-repo. Do **not** say
"I can't flash for you" — that assumption came from a sandboxed chat and no longer applies.

The only steps that still need the human:
- physically **holding the G0 button** on the Stamp to enter download mode,
- plugging / unplugging USB,
- observing on-screen / hardware behavior and reporting back.

Windows terminal: prefer **PowerShell**. CMD mangles multi-line commands and quotes — use
`.ps1` / `.sh` scripts or PowerShell for anything nontrivial.

## What Draupnir is
A self-contained **USB-HID macro controller**. Phase-1 POC = two pucks cabled over I2C:
- **M5Stack Dial** (ESP32-S3, 1.28" round touch screen, rotary encoder) — brain, screen, USB HID device.
- **Adafruit NeoTrellis 4x4** (16 elastomer keys, per-key RGB, seesaw over I2C) — key bank.

Unifying model: **dial detent N = key N = color N = icon N.** 16 keys = random-access macro
firing; each lit in its macro's color as a legend; the round screen shows the selected macro.
Pure USB HID at runtime (no host app); configured via a browser over the Dial's Wi-Fi;
macros stored in Dial flash.

## Decisions locked
- **Firmware:** Arduino framework + M5Unified (+ LVGL or M5GFX, TinyUSB HID, LittleFS, ESPAsyncWebServer).
- **Macro engine:** sequences + delays (key combos, text, consumer/media, mouse, delays). No on-device scripting in v1.
- **Config:** Wi-Fi web UI writes `profiles.json` to LittleFS. No installed app.
- **Storage:** `profiles.json` in LittleFS; last profile + brightness in NVS.
- **Open source** (firmware + hardware), MIT.

## Toolchain — arduino-cli (agent-driven, primary path)
Build and flash headlessly from the terminal:
```
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
arduino-cli core update-index
arduino-cli core install m5stack:esp32
arduino-cli lib install M5Dial            # pulls M5Unified + M5GFX
arduino-cli board listall | grep -i dial  # discover the exact M5Dial FQBN (don't guess it)
# user holds G0, plugs USB, releases -> download mode, then:
arduino-cli compile --fqbn <FQBN> firmware/M0_bringup
arduino-cli upload -p <PORT> --fqbn <FQBN> firmware/M0_bringup
arduino-cli monitor -p <PORT> -c baudrate=115200
```
For **M1 (USB HID)**, USB mode is a board-menu option: inspect with
`arduino-cli board details --fqbn <FQBN>` and append the USB-OTG / TinyUSB option to the
FQBN (e.g. `<FQBN>:USBMode=...`). Arduino IDE GUI remains a fallback — see
`docs/M0_Setup_and_BringUp.md` and `docs/Toolchain_arduino-cli.md`.

## Roadmap
- **P1 POC (now):** M5Dial + NeoTrellis; prove firmware + interaction.
- **P2:** custom radial or Megalodon-style PCB; hotswap mechanical keys + per-key RGB.
- **P3 (stretch):** sellable product; build on a **pre-certified ESP32-S3 module** to inherit
  radio FCC/CE; ship via Tindie / Crowd Supply; keep it open.

## Current status
**M6 (Wi-Fi Config + Mobile App)** — Firmware now includes `CONFIG_MODE` with token-based authentication via a Flutter mobile Companion App. Profiles are persisted to LittleFS. USB HID macros are fully functional. **Next = M7** polish, testing, and documentation.

## Key hardware facts
**M5Dial:** StampS3 (ESP32-S3FN8), 8MB flash, **no PSRAM, no SD**. Encoder G40/G41, screen
GC9A01 240x240, touch FT3267, buzzer G3, native USB G19/G20, HOLD G46. PORT.A = I2C
(G13/G15) → NeoTrellis; PORT.B = GPIO (G2/G1) free. Download mode: hold **G0** on the back
Stamp, plug USB-C, release.
**NeoTrellis (PCB 3954):** seesaw I2C, default **0x2E**; silicone button pad bought
separately; **STEMMA (JST-PH)** connector → needs a Grove↔STEMMA adapter to the Dial;
5V over Grove for the NeoPixels; cap LED brightness.

## Working style
Incremental milestones, each verified **on hardware** before advancing. You compile/upload
directly; the human supplies the G0 press and reports what the screen / Serial shows. Keep
the macro + config data model hardware-agnostic so the form factor can change (P2/P3)
without breaking profiles.
