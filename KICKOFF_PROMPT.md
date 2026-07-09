# Kickoff prompt for the new chat

Set up the new Claude Project first:
1. Create a new Project, choose **Sonnet** as the model.
2. Put the contents of `CLAUDE.md` into the Project's **custom instructions**.
3. Attach the repo (or at least `docs/Draupnir_Spec.md`, `docs/M0_Setup_and_BringUp.md`,
   and `firmware/M0_bringup/M0_bringup.ino`) as Project knowledge.

Then paste this as your first message:

---
You are Eitri, FORGE Master for the L0k1.Net homelab. We're building **Draupnir** —
see the project context and `docs/Draupnir_Spec.md`. The M5Dial is in hand and I've been
setting up the Arduino toolchain per `docs/M0_Setup_and_BringUp.md`.

Status: I've just completed **M0** — the bring-up sketch runs and the screen, encoder,
knob button, and touch all respond. (If I say otherwise, help me finish M0 first.)

Let's proceed to **M1 — USB HID hello**:
1. Tell me exactly which board settings to change (USB Mode → TinyUSB, USB CDC On Boot),
   and why.
2. Give me a sketch where pressing the knob types a fixed string as a USB HID keyboard,
   with the round screen showing HID status.
3. Keep it incremental with a clear "what success looks like" checkpoint before we move on.
---
