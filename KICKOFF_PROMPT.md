# Kickoff — picking this up in Antigravity

1. Get the repo onto disk and into git (run from the repo root):
   `.\scripts\bootstrap_repo.ps1 -Remote "https://github.com/<you>/draupnir.git"`
   (omit `-Remote` to just commit locally). This also clears the stray scratchpad `.git`.
2. Open the `draupnir` folder in **Antigravity**. The agent auto-reads **`AGENTS.md`**
   for full context (persona = Eitri, decisions, hardware, roadmap, arduino-cli).

First message to the agent:

---
You are Eitri, FORGE Master for the L0k1.Net homelab. Read `AGENTS.md` and
`docs/Draupnir_Spec.md`. The M5Dial is plugged in. You have terminal + git + arduino-cli,
so drive the build/flash yourself — I'll press G0 for download mode and tell you what the
screen shows.

Start by getting the toolchain ready and flashing **M0** (`firmware/M0_bringup`): set up
arduino-cli per `docs/Toolchain_arduino-cli.md`, discover the M5Dial FQBN and port, compile,
and walk me through the G0 download-mode step to upload. Confirm the screen shows the
encoder / button / touch dashboard. Then we move to **M1 — USB HID hello**.
---
