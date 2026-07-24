# Kickoff — picking this project up in a fresh agent session

The agent auto-reads **`AGENTS.md`** (Antigravity) or **`CLAUDE.md`** (Claude Code) for full
context: persona = Eitri, locked decisions, board facts, threading rules, arduino-cli.
Those two files are copies of each other — edit both together.

Read `docs/Draupnir_Spec.md` (v3) before any design work. It supersedes the two-puck /
Wi-Fi-web-UI design that earlier docs and commits describe.

First message to the agent:

---
You are Eitri, FORGE Master for the L0k1.Net homelab. Read `AGENTS.md` and
`docs/Draupnir_Spec.md`. You have terminal + git + arduino-cli, so drive the build/flash
yourself — I'll plug in the board, press G0 if it's the M5Dial, and tell you what the screen
and Serial show.

Current state: USB HID, the macro engine, the ring UI, the LittleFS profile store, and BLE
config all work on hardware. Next up is **M6 — config hardening**, which is blocking:
enforce BLE pairing on the config characteristics, make profile writes atomic with a
parse-failure fallback, bound the BLE RX reassembly buffer, and stop running macros before a
profile reload.

Start by reading `firmware/Waveshare_LVGL_Test/ble_engine.cpp` and `macro_engine.cpp`, then
propose the M6 change set before touching code.
---

## Notes for the first session

- **Untracked work.** Chunks of the Waveshare firmware may still be untracked. Check
  `git status` and commit before anything destructive.
- **Primary board** is the Waveshare ESP32-S3 knob (`firmware/Waveshare_LVGL_Test/`). The
  M5Dial (`firmware/M5_M6_config/`) is a supported second target and still contains the
  legacy web server + token pairing that v3 removes.
- **Repo bootstrap**, if starting from a bare copy:
  `.\scripts\bootstrap_repo.ps1 -Remote "https://github.com/<you>/draupnir.git"`
  (omit `-Remote` to just commit locally).
