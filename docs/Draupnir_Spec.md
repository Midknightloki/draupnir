# Project Draupnir — Dial + NeoTrellis Macro Controller

**A two-puck, self-contained HID macro controller: a rotary-knob brain + a 16-key RGB pad.**

*A FORGE project spec · L0k1.Net · v2, drafted 2026-06-27*

> Codename note: *Draupnir* is the golden ring the dwarf Eitri forged that drips eight new rings every ninth night. A round controller that multiplies your macros felt apt. Rename at will.

> **v2 change:** the key bank is now an Adafruit **NeoTrellis** 4x4 (16 buttons, per-key RGB, I2C) in its own acrylic enclosure, cabled to the M5Dial. This gives full random-access to 16 macros, a color-coded legend that mirrors the dial's 16 detents, and zero fabrication — both pucks come enclosed.

---

## 0. North Star & roadmap

**End goal:** a compact, distinctive **radial** macro controller — 16 hotswap mechanical keys with per-key NeoPixels arranged in a ring around the central encoder + round screen — polished enough to be a **sellable product.**

Why this is the target: the radial layout maps perfectly to the encoder's 16 detents (key angle = dial angle), it's visually unlike the sea of square macropads, and it's the natural home for everything the firmware already does. The market precedent for a solo-maker open macropad is real (duckyPad on Tindie; DeepDeck on Crowd Supply).

**The phases share one firmware.** The `pos` 0-15 model, color legend, key/LED loop, HID engine, and profile system are identical across every phase. Only the physical key arrangement and wiring change. So earlier phases de-risk the expensive one.

| Phase | Build | Purpose |
|---|---|---|
| **P1 — POC (now)** | M5Dial + NeoTrellis, two pucks cabled (this spec) | Prove firmware + interaction on cheap, enclosed, off-the-shelf parts. No fab. |
| **P2 — Personal radial** | Custom KiCad PCB: 16 Kailh hotswap sockets + reverse-mount NeoPixels in a ring, center EC11, ESP32-S3 module, round screen in the hub. Laser-cut/printed enclosure. | One-off "real" device for myself; validate the radial form + manufacturing. |
| **P3 — Product (stretch)** | Refined P2 + DFM, enclosure tooling, packaging, docs | Small-batch sellable unit. |

**Design constraints to honor from P1 so P3 stays open:**
- Build around a **pre-certified ESP32-S3 module** (e.g. StampS3 / ESP32-S3-WROOM) so radio FCC/CE modular approval is largely inherited — turns certification from a blocker into a checkbox.
- Keep **firmware + hardware open source** (duckyPad playbook) — values fit *and* community/marketing flywheel.
- Likely go-to-market lanes: **Tindie** (sell-as-you-fab) and/or **Crowd Supply** (crowdfund + fulfillment).
- Keep the macro data model and config UI hardware-agnostic so a buyer's profiles survive form-factor changes.

> P2/P3 are explicitly out of scope until P1 proves the concept. Everything below is P1 unless noted.

---

## 1. Concept

Two pucks that cable together:

- **Dial puck — M5Stack Dial:** the brain. ESP32-S3, 1.28" round touch screen, and the rotary encoder (16 detents). Runs the firmware, holds the macros, and is the USB HID device.
- **Key puck — Adafruit NeoTrellis 4x4:** 16 elastomer buttons, each with its own RGB NeoPixel, all over I2C. No brain of its own — it's a peripheral the Dial reads and lights.

The unifying idea: **dial position N = key N = color N = icon N.** The encoder's 16 detents map 1:1 to the 16 keys. Every key is lit in its macro's assigned color as a always-on legend; the round screen shows the selected macro's icon/name. You can rotate to highlight and press the knob, **or** just hit the physical key directly — full random access. Turn past the set or swipe to change profiles, and all 16 keys plus the screen re-legend with the new colors.

It runs as a standard USB HID device — no host software at runtime. Macros live in the Dial's flash; configuration is a browser over the Dial's own Wi-Fi.

### What it is
A desk macro controller with a color-coded 16-key pad and a screen+knob brain, fully enclosed, no 3D printing.

### What it is not (v1 non-goals)
- Not a duckyScript interpreter (sequences + delays, no loops/variables on-device).
- No microSD (the M5Dial has none) — profiles live in internal flash.
- NeoTrellis buttons are soft silicone, not mechanical/hotswap (see §11 for the hotswap path).

---

## 2. Goals & success criteria

1. Plug-and-play **USB HID** keyboard/mouse/media device — works at boot, no drivers.
2. **16 physical keys = instant random access** to the active profile's macros.
3. **Color-coded legend:** each key lit in its macro's color; screen shows the selected macro's icon/name; dial detent aligns with key.
4. **Multiple profiles**, each up to 16 macros, persisted in onboard flash; switching re-legends keys + screen.
5. Macros support **key combos, typed text, media/consumer keys, basic mouse, and inter-step delays.**
6. Configurable from **any browser** via the Dial's Wi-Fi config page — no installed app.
7. Survives power cycles; boots into the last-used profile with the right colors.

**Done when:** I configure a profile in the browser, unplug/replug, see the 16 keys light in their colors, press a key, and the keystrokes land in the focused app — config page closed.

---

## 3. Hardware

### Dial puck — M5Stack Dial v1.1
| Item | Detail |
|---|---|
| Controller | M5StampS3 — ESP32-S3FN8, dual-core LX7 @240MHz, native USB (OTG/CDC) |
| Flash | 8 MB (no PSRAM, no SD) |
| Display | 1.28" round IPS, GC9A01, 240x240, FT3267 touch |
| Input | Rotary encoder (16 detents / 64 PPR) + front/knob button |
| Extras | Buzzer, RTC, Wi-Fi 2.4 GHz |
| Power | USB-C 5V (also JST LiPo / 6-36V terminal); HOLD latch G46 |
| Ports | PORT.A (Grove, I2C, G13/G15) <- used for NeoTrellis; PORT.B (Grove, GPIO, G2/G1) free |
| Enclosure | Finished case, 51x51x32 mm |

### Key puck — Adafruit NeoTrellis (PCB 3954)
| Item | Detail |
|---|---|
| Buttons | 4x4 = 16, elastomer silicone pad (**bought separately** — PCB 3954 has no buttons) |
| LEDs | 16x WS2812 NeoPixel, one under each key |
| Brain | seesaw (ATtiny-class) — keypad scan + LED drive over I2C |
| Interface | I2C, 7-bit address 0x2E-0x4D (solder-jumper selectable); default 0x2E |
| Connector | STEMMA (JST-PH 4-pin) — **not** Grove and **not** STEMMA QT |
| Size | 60 x 60 x 7.5 mm; acrylic enclosure available |
| Libraries | Adafruit Seesaw / NeoTrellis (Arduino + CircuitPython) |

### Connecting the two
- M5Dial **PORT.A** (Grove HY2.0 I2C) -> **Grove-to-STEMMA (JST-PH) adapter cable** -> NeoTrellis.
- Grove supplies **5V**, which the NeoPixels prefer for brightness; the Dial's 3.3V I2C logic is fine with the seesaw.
- NeoTrellis default address 0x2E; no conflict with the Dial's onboard I2C devices, but confirm on first scan.
- PORT.B stays free for a future second encoder or Grove keys (§12).

---

## 4. Interaction model

**Run mode (the color-coded macro grid):**
- **All 16 keys lit** in their macros' colors = persistent legend. Empty slots dim/off.
- **Screen** shows the active profile name + the selected macro's big icon/name; optionally a 16-dot ring/grid mirroring the key colors.
- **Press any NeoTrellis key** -> fire that macro immediately (random access). Key flashes + buzzer blip to confirm.
- **Rotate dial** -> move the selection highlight across the 16 (detent-aligned); screen updates; selected key brightens/pulses.
- **Press knob** -> fire the currently selected macro (alternative to touching the key).
- **Touch swipe / long-press menu** -> switch profile or page; keys + screen re-legend with new colors.
- **Optional dial-as-volume** per profile (turn = volume, key press still fires).

This unifies the whole device: dial detent <-> screen icon <-> physical key <-> color, all pointing at the same macro.

---

## 5. Firmware architecture (Arduino + M5Unified)

```
+-----------------------------------------------+
| App / state machine (run <-> menu <-> config) |
+-------------+-------------+-------------+------+
| UI layer    | Input layer | Key puck    | HID  |
| LVGL round  | M5Dial:     | NeoTrellis  | Tiny |
| screen +    | encoder,    | over I2C:   | USB  |
| color ring  | button,     | read keys + | kbd/ |
|             | touch       | drive 16 LED| mouse|
+-------------+-------------+-------------+------+
| Macro engine (executes action sequences)      |
+-----------------------------------------------+
| Store: LittleFS (profiles.json) + NVS (state) |
+-----------------------------------------------+
| Config server: Wi-Fi AP + ESPAsyncWebServer   |
+-----------------------------------------------+
```

**Key libraries**
- `M5Dial` / `M5Unified` — display, encoder, button, touch, buzzer.
- `Adafruit_NeoTrellis` (+ `Adafruit_seesaw`) — read key events, set per-key NeoPixel colors over I2C.
- `LVGL` (v8/v9) — round UI (or M5GFX directly if LVGL feels heavy at 240x240).
- Arduino ESP32 USB: `USBHIDKeyboard`, `USBHIDMouse`, `USBHIDConsumerControl` (TinyUSB).
- `LittleFS` (macro store) + `Preferences`/NVS (last profile, brightness).
- `ESPAsyncWebServer` + `AsyncTCP` + `ArduinoJson` (config UI).

**Main loop**
1. `M5Dial.update()` -> encoder delta, knob button, touch.
2. `trellis.read()` -> NeoTrellis key events.
3. Apply input to UI state (selection / profile / mode).
4. On any key press (physical or knob) -> `MacroEngine.run(macro)`.
5. On profile change -> repaint screen + push 16 colors to NeoTrellis.
6. Render screen only on change.

**Modes**
- **Run:** USB HID active, Wi-Fi off; NeoTrellis lit + scanning.
- **Config:** long-press -> Wi-Fi AP + web server up, HID idle; exit reloads `profiles.json` and re-legends.

---

## 6. Macro data model (sequences + delays)

`profiles.json` in LittleFS. Each macro now carries a `color` for its key LED and a grid position.

```json
{
  "version": 2,
  "activeProfile": 0,
  "settings": { "brightness": 80, "ledBrightness": 60, "buzzer": true },
  "profiles": [
    {
      "name": "Editing",
      "macros": [
        { "pos": 0,  "name": "Build",   "icon": "hammer", "color": "#E0A030",
          "actions": [ { "type": "key", "mods": ["CTRL","SHIFT"], "key": "B" } ] },
        { "pos": 1,  "name": "Sign-off","icon": "text",   "color": "#3080E0",
          "actions": [ { "type": "text", "value": "- Eitri, FORGE Master\n" } ] },
        { "pos": 2,  "name": "Mute",    "icon": "mic",    "color": "#E03030",
          "actions": [ { "type": "consumer", "code": "MUTE" } ] },
        { "pos": 3,  "name": "Shot+paste", "color": "#30C060",
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

`pos` = 0-15 grid index (maps to both the NeoTrellis key and the dial detent). `color` = key LED. Action types unchanged:

| type | fields | notes |
|---|---|---|
| `key` | `mods[]`, `key` | modifier combo + key |
| `text` | `value` | typed string (US layout for v1) |
| `delay` | `ms` | pause between steps |
| `consumer` | `code` | VOL_UP/DOWN, MUTE, PLAY_PAUSE, NEXT, PREV |
| `mouse_move` | `dx`,`dy` | relative move |
| `mouse_click` | `button` | LEFT/RIGHT/MIDDLE |

---

## 7. Config: Wi-Fi web UI

- Long-press -> Config mode: Dial starts a SoftAP (e.g. `Draupnir-XXXX`, QR on screen) or joins your LAN.
- Browser -> single-page config app (served from firmware/LittleFS).
- Edit profiles and macros, assign **grid position + color + icon**, build action sequences, set brightness (screen + LED).
- Save -> POST JSON -> validate (ArduinoJson) -> write `profiles.json` -> reload + re-legend.
- Add an **export/import** button so you can back up `profiles.json` (no SD card).

---

## 8. Storage & power

- **Macros/profiles:** `profiles.json` in LittleFS.
- **State:** last profile + brightness in NVS (`Preferences`), restored on boot.
- **Power:** USB-C powers both pucks (NeoTrellis draws 5V over the Grove/STEMMA link). With 16 NeoPixels lit, mind total current — cap `ledBrightness` and avoid all-white-full. LiPo on the Dial is possible but the NeoTrellis LEDs will dominate battery draw; treat battery use as desk-untethered bursts, not all-day.

---

## 9. Dev environment

- **Arduino IDE or PlatformIO**, ESP32 Arduino core (S3).
- Board profile: M5Stack StampS3 / M5Dial; set **USB-OTG / "USB CDC On Boot"** so HID + flashing coexist.
- Libraries: `M5Dial`, `M5Unified`, `Adafruit_NeoTrellis`, `Adafruit_seesaw`, `LVGL`, `ESPAsyncWebServer`, `AsyncTCP`, `ArduinoJson`.
- Flash via G0 download mode (hold G0, power on, release).
- Refs: M5Dial Arduino quick-start; Adafruit NeoTrellis guide.

---

## 10. Milestones

| # | Milestone | Outcome |
|---|---|---|
| M0 | Dial bring-up | Stock example runs; screen, encoder, knob, touch read. |
| M1 | HID hello | Dial enumerates as USB keyboard; knob press types a fixed string. |
| M2 | NeoTrellis bring-up | Over I2C: read all 16 keys + set all 16 colors. Confirm address + Grove/STEMMA cable. |
| M3 | Keys fire HID | Press a NeoTrellis key -> fires a multi-step macro; key flashes. |
| M4 | Color legend + dial | 16 keys lit per macro color; dial highlight tracks selection; knob fires selected. |
| M5 | Store + profiles | Load `profiles.json`; profile switch re-legends keys + screen; boot into last (NVS). |
| M6 | Web config | Config mode serves the page; edits (incl. position/color) save + reload live. |
| M7 | Polish | Paging, icons, brightness caps, buzzer feedback, export/import, (battery). |

Dogfood gate at **M4**: once the lit grid + dial + HID firing all work together, use it for real before building config/profiles. This is the moment the whole concept proves out.

---

## 11. Tradeoffs (the random-access problem is now solved)

With 16 physical keys you get **instant random access** — the serial-scroll limitation of the dial-only design is gone. The dial becomes a selector/profile/volume control rather than the only way in. Remaining notes:

- **>16 macros** still means multiple **profiles** (switch via touch/long-press); the color legend makes context switches legible.
- **Soft buttons, not mechanical.** NeoTrellis is silicone elastomer — pleasant and quiet, but not clicky/hotswap. If mechanical feel becomes a must-have, see §12.

---

## 12. Expansion path (optional)

- **Hotswap mechanical variant:** swap the NeoTrellis puck for **4x Adafruit NeoKey 1x4** (Kailh hotswap MX + NeoPixel, I2C, chainable to 16 keys on one bus). Same firmware key/LED model, just different I2C devices and a plate to hold the four strips. This is the mechanical+hotswap+RGB+I2C path if you want it later.
- **Second encoder / extra keys on PORT.B:** the Dial's GPIO port is still free for an EC11 or a couple of switches.
- **Tile more NeoTrellis:** the seesaw addressing supports many boards — an 8-key or 4x8 layout is possible if 16 ever feels tight.

---

## 13. Risks & open questions

- **Buy the silicone pad:** NeoTrellis PCB 3954 ships **without buttons** — add the 4x4 silicone elastomer pad (and the acrylic enclosure) to the order.
- **Connector mismatch:** Grove (M5) vs STEMMA JST-PH (NeoTrellis) — get the adapter cable; don't assume the plugs mate.
- **NeoPixel current:** 16 LEDs at full white can pull real current over a thin Grove cable; keep `ledBrightness` sane.
- **Keyboard layout:** HID sends keycodes; typed `text` assumes US layout for v1.
- **USB routing / CDC-on-boot:** confirm native USB on the Dial's USB-C and set CDC-on-boot so you can reflash with HID firmware loaded (else G0 download mode).
- **I2C address scan:** verify NeoTrellis at 0x2E and no clash with onboard Dial I2C before wiring assumptions into firmware.

---

## 14. Shopping list

- **M5Stack Dial v1.1** — brain + screen + encoder. *(ordered)*
- **Adafruit NeoTrellis PCB (3954)** — 16-key RGB pad driver.
- **4x4 silicone elastomer button pad** (Adafruit) — the actual buttons; pick your color.
- **Acrylic enclosure for NeoTrellis.**
- **Grove-to-STEMMA (JST-PH) adapter cable** — Dial PORT.A to NeoTrellis.
- USB-C data cable.
- *(Optional, later)* 4x NeoKey 1x4 for the hotswap-mechanical variant (§12).

Two pucks, one cable, no printer, no soldering (beyond optional address jumpers).

---

## Appendix — Inspiration & parking lot

*Not committed scope. Ideas to revisit at P2/P3 once the POC proves the firmware and interaction. Nothing here affects the current build.*

### A. Form-factor references (decide at P2)

Two silhouettes worth keeping on the shelf, held as equal candidates — no ranking yet:

- **Radial ring.** 16 hotswap NeoPixel keys in a circle around the central smart knob/screen. Striking and novel; key angle maps directly to dial angle. Harder to route and manufacture; cramped.
- **Megalodon-style asymmetric cluster.** Inspired by the DOIO KB16 Megalodon: a key grid offset from a knob cluster (one primary + secondary encoders) with a screen. Ergonomic and easy to manufacture (rectangular PCB, standard grid). The DOIO is itself open (QMK/VIA) and hotswap, so it's a free reference design, *and* market validation that the silhouette sells.

**Smart-knob upgrade (applies to either silhouette):** replace a plain encoder with a knob that has its own screen.
- *Pragmatic:* M5Dial-style round LCD + normal encoder (what the POC already uses) — context-aware knob, ~zero added complexity.
- *Halo:* a haptic SmartKnob (Scott Bezek's open Apache-2.0 SmartKnob View — BLDC motor + magnetic encoder + 240x240 screen + strain-gauge press) giving software-defined detents/endstops that change feel per context. Big engineering jump (FOC motor control, more current, bulkier), but a standout differentiator. Seeed sells a SmartKnob DevKit to prototype it without building the motor assembly.

### B. Premium per-key feedback — LCD screen-buttons (alt product, not a variant)

Each key is its own small color LCD bonded to a switch (e.g. 0.85" 128x128 SPI display-buttons), Stream-Deck-style — every key shows an arbitrary dynamic icon/label instead of just a color.

- **Why it's tempting:** richest possible per-key feedback; keys self-describe with no central screen needed.
- **Why it's hard / not the POC:**
  - *SPI, not I2C* — won't sit on the Grove port; needs a chip-select per display (16 CS lines or a mux), breaking the single-cable model.
  - *RAM/bandwidth* — 128x128x16bpp ~= 32KB/image, ~512KB for 16; the StampS3 has no PSRAM, so icons must stream from flash per change.
  - *Cost* — ~$9 each, ~$145 for sixteen (≈10x the NeoTrellis pad).
  - *Redundant with the hub screen* — per-key LCDs earn their cost only when there's **no** central screen; a dial-centric device already shows the selection in the hub.
- **Where it fits:** a separate, more ambitious SKU — a screenless-hub, all-LCD-key deck where every key speaks for itself. P3-class effort (multi-SPI driver, flash asset pipeline, icon-upload tool). Filed as its own product idea, not a Draupnir variant.

### Reference links
- DOIO KB16 Megalodon (KeebMonkey); alternate QMK firmware (wlellington/frogimancer-macropad)
- SmartKnob (scottbez1/smartknob, Apache-2.0); SmartKnob DevKit (SeedLabs)
- Adafruit NeoKey 1x4 (hotswap I2C path); Adafruit NeoTrellis (current POC key bank)
