# Draupnir

An open, self-contained **USB-HID macro controller**: a rotary-knob brain plus a
16-key RGB pad, with a roadmap toward a custom radial / premium form factor.

> *Draupnir* — the golden ring the dwarf Eitri forged that drips eight new rings
> every ninth night. A round controller that multiplies your macros.

**Status:** Phase 1 (proof of concept) — milestone **M0**, hardware bring-up.

## Concept
One mental model: **dial detent N = key N = color N = icon N.**
- Turn the knob to highlight; press a physical key (or the knob) to fire a macro.
- All 16 keys are lit in their macros' colors as a persistent legend.
- The round screen shows the active profile and the selected macro.
- Standard USB HID — no host software at runtime. Macros live in flash;
  configuration is a browser over the device's own Wi-Fi.

## Hardware (POC)
| Puck | Part | Role |
|------|------|------|
| Brain | M5Stack Dial (ESP32-S3, 1.28" round touch, encoder) | firmware, screen, USB HID |
| Keys | Adafruit NeoTrellis 4x4 (16 keys + per-key RGB, I2C) | key bank + color legend |

Cabled M5Dial **PORT.A (I2C)** → Grove↔STEMMA adapter → NeoTrellis. See the spec for details.

## Firmware stack
Arduino + M5Unified · LVGL/M5GFX · TinyUSB HID · LittleFS · ESPAsyncWebServer.
Macro engine = sequences + delays (keys, text, media, mouse). No on-device scripting in v1.

## Repo layout
```
docs/    Draupnir_Spec.md          full spec (authoritative)
         M0_Setup_and_BringUp.md   toolchain + first flash
firmware/M0_bringup/               M0 bring-up sketch
scripts/                           helper scripts (git push, etc.)
CLAUDE.md                          context for the AI assistant (Eitri)
```

## Getting started
See **docs/M0_Setup_and_BringUp.md** — install the toolchain, flash M5's example,
then flash `firmware/M0_bringup`.

## Roadmap
- **P1 — POC (now):** M5Dial + NeoTrellis; prove firmware + interaction.
- **P2:** custom radial / Megalodon-style PCB; hotswap mechanical + RGB.
- **P3 (stretch):** sellable product; build on a pre-certified ESP32-S3 module
  to inherit radio cert; ship via Tindie / Crowd Supply; stay open.

## License
MIT — see [LICENSE](LICENSE). Hardware designs intended to be open as well.
