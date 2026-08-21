# Draupnir — Session Handoff

*Written 2026-08-20. Pick up here.*

This is a continuation brief for whoever works on Draupnir next — another agent, a fresh session,
or the owner. It assumes **no prior context**.

---

## 1. Read these first

| Document | What it gives you |
|---|---|
| `CLAUDE.md` / `AGENTS.md` | Persona, locked decisions, threading rules, FQBNs. Identical copies — **edit both together.** |
| `docs/Draupnir_Spec.md` (v3) | The brief. Concept, hardware, data model, BLE protocol, milestones (§10 has the current state). |
| `docs/superpowers/specs/2026-08-07-m7-m8-persistence-design.md` | The design this milestone was built against. |
| `docs/superpowers/plans/2026-08-07-m7-m8-persistence.md` | The task-by-task implementation plan. |
| `.superpowers/sdd/2026-08-07-m7-m8-persistence/progress.md` | The full execution ledger for this milestone — every task, review finding, hardware checkpoint, and owner decision, in order. This document is a summary of it; the ledger is the source of truth. |
| `docs/Toolchain_arduino-cli.md` | **Read before touching hardware.** Board quirks below will otherwise cost you hours. |

---

## 2. The goal

Draupnir is a **USB-HID macro controller in a knob** — round touch screen plus rotary encoder,
driverless over USB HID, configured from a Flutter phone app over BLE. Macros show as a ring of
colored wedges; rotate to select, tap center or tap a wedge to fire.

**M6 (config hardening) is done.** The BLE config channel now refuses an unbonded central at the
GATT layer — verified on hardware 2026-08-07. **M7 (NVS persistence) and M8 (on-device profile
switching) are also done**, verified on hardware, on branch `feat/m7-m8-persistence`. M8 grew well
past its original scope into a full ring rework; see §3.

**The immediate goal is finishing this branch.** Code is written, reviewed, flashed, and the
final-review hardware pass is complete (§4) — all six checks passed. The sequence from here is
M8b/M9 (icons — the ring is now shaped to receive them) and then closing the M5Dial's security
gap, ahead of M10's comfort items. See §6.

---

## 3. Where things stand

Branch `feat/m7-m8-persistence`, 28 commits ahead of `review/waveshare-m6-foundation` (base
`a254067`, the M6 foundation branch — not `main`, which is further behind). Nothing on this branch
has been merged yet.

Highlights, oldest to newest (full detail, including every review round and hardware checkpoint,
is in the ledger at `.superpowers/sdd/2026-08-07-m7-m8-persistence/progress.md`):

```
c21baac  feat(ui): Orbitron replaces Montserrat on the ring
0ee54d4  feat(m7): device_state owns NVS; brightness applied at boot
c16e77f  fix(m7): use Preferences::isKey() instead of a sentinel value
b1c7f5e  feat(m7): Settings menu -- swipe up opens and kills macros, swipe down closes
c256725  feat(m7): on-device brightness with a half-moon gauge
175d3b3  fix(m7): settings panels were swallowing every tap
085ddc6  fix(m7): swipe-up's release re-fires the macro settings just stopped
8ef54da  feat: ship a second default profile so switching is testable
1b335f4  feat(m8): on-device profile switching, and activeProfile finally gets a writer
3fa8ece  feat(m8): directional profile indicators, tappable
ba32d42  fix: tune LVGL gesture thresholds so swipes actually register
01d6829  fix: clear LV_OBJ_FLAG_SCROLLABLE on the ring screen -- horizontal swipes never gestured
2b3551e..e0ecec5  chevron indicator iterations (superseded by Task 8's glyph, below)
783fab0  feat(waveshare): ring rework -- rotating ring, centre label stack, tinted bloom
6b37127  fix: ring rework round 2 -- animated rotation, tap-fires-no-select, wedge names
9c85c57  feat(ring): lit-wedge selection, outlined labels, crisp bloom ring
e68b017, afcecb7  tune: ring rotation easing, per owner hardware feedback (0.30 -> 0.45 -> 0.58)
1a36d53  fix: replace lossy knob event group with a counting queue
b2a7aa3  fix: quadrature phase decode -- REVERTED in 3ada93d, killed the dial entirely
f32a255  diag: raw-pin ring buffer for double-detent investigation
cdfd3b5  fix: saturate knob debounce counter -- itself buggy, see C1 below
88ed6f2  fix: whole-branch review findings (C1/C2/I1/I2/M3/M4/M6/M7/M9)
5c6d52c  fix: budget the ellipsis width in wedge_label_fit() before truncation
9ed000a  fix: stop uppercase key letters from silently injecting Shift    <- HEAD
```

Everything through `9ed000a` (HEAD) has now been flashed and hardware-tested — the outstanding
hardware pass in §4 completed 2026-08-20, all six checks passing. See §4 for exactly what that
pass did and did not exercise.

---

## 4. Verified vs. not verified on hardware

**This is the most important section in this document.** Do not build on anything listed as "not
verified" as though it were confirmed.

### Verified on hardware during this milestone

- Orbitron rendering on the ring.
- Brightness: JSON seed → NVS → boot → panel. The screen is visibly dimmer at a low stored value,
  and `settings.brightness` — present in the schema since the first profile store and never
  wired to anything — now does something.
- The Settings menu and the brightness gauge, **including tap-to-enter and tap-to-confirm** (this
  was a Critical finding: `lv_obj_create()` sets `LV_OBJ_FLAG_CLICKABLE` by default, and the
  settings panels never cleared it, so a full-screen container was silently stealing every tap).
- Exactly one NVS write per Settings session, not one per detent (`Preferences::isKey()` guards
  it) — confirmed by serial log, and a second edit session with no change produced no write.
- Macros stopping when Settings opens, **and staying stopped.** This took two rounds: the first
  fix stopped the macro but a queued fire from the finger's release event immediately restarted
  it; the second drained the pending-fire queue as part of `macros_stop_all()`.
- Profile switching: persists across a power cycle, survives a switch away from a *running*
  toggle macro with no crash and no `rst:0x` anywhere in the capture.
- Swipes registering reliably, both directions. This needed two independent fixes layered on top
  of each other — tuned gesture thresholds (`ba32d42`) and clearing `LV_OBJ_FLAG_SCROLLABLE` on
  the ring screen (`01d6829`, the actual root cause: label overflow was making the screen
  scrollable, and a scrolling object eats gestures before LVGL ever checks thresholds).
- The complete ring rework (Tasks 8–10): rotating ring under a static 12 o'clock selector, centre
  label stack, tinted selection bloom, outlined wedge labels, FontAwesome chevron indicators. The
  owner's own words on seeing it: **"This looks great."**
- Heap flat throughout — no drift attributable to a leak was observed at any checkpoint.
- **The final-review fix wave and the phantom-Shift fix, confirmed 2026-08-20** (§4's "outstanding
  hardware pass" below has the full record): a long knob hold still produces a detent; a no-op
  swipe-down no longer fires a macro; a refused swipe inside Settings no longer activates a menu
  item; a long macro name truncates with an ellipsis instead of smearing; brightness survives a
  power cycle with the NVS write outside `lvgl_lock()`; and an existing `Win+L` macro now works
  without being re-created, on both firmware and app.

### NOT verified on hardware — do not imply otherwise

- **Task 5's built-in second default profile.** It only regenerates on a wiped device or a
  missing/corrupt `profiles.json`, and the owner deliberately chose to add a second profile from
  the companion app instead, to keep their real macros. The regeneration path was validated by two
  independent code reviews (JSON correctness, consumer codes checked against
  `getConsumerCode()`), never exercised on the device.
- **The debounce-counter fix's actual overflow path.** See the "outstanding hardware pass" section
  below — the long-hold check passed, but it did not exercise the bug the fix targets.
- **Frame pacing at 12–16 macros.** All hardware testing this milestone ran against a 4-macro
  profile. The ring's draw callback issues up to 9 `lv_draw_label` calls per wedge per repaint, and
  the final review turned up a detail that makes this worse than it first looked:
  `EXAMPLE_LVGL_BUF_HEIGHT` is `V_RES / 10`, so `ring_draw_event_cb` runs once per render *stripe*,
  not once per frame — roughly **10× the earlier back-of-envelope estimate**. Documented fallback
  is dropping to 4 label offsets if it stutters. Nobody has loaded a 12+ macro profile onto the
  device yet.

### Outstanding hardware pass — completed 2026-08-20, all six passed

All six checks below were run against a build of `9ed000a` and passed (owner, 2026-08-20). This
confirms the final-review fix wave (`88ed6f2`, `5c6d52c` — gesture-release leak, label truncation,
the NVS-lock change) and the phantom-Shift fix (`9ed000a`) on both the firmware and app sides.

1. A long knob hold (≥ 1 s) still produces a detent (the debounce-counter fix, `88ed6f2`). **PASS
   — but read the next paragraph before treating the fix itself as exercised.**
2. A no-op swipe-down (nothing running) does **not** fire a macro. *This previously sent
   keystrokes to the host* — a real regression, not a theoretical one. **PASS.**
3. A refused swipe inside Settings does **not** activate a menu item. **PASS.**
4. A long macro name truncates with an ellipsis rather than smearing across the neighbouring
   wedge (`5c6d52c`). **PASS.**
5. Brightness still survives a power cycle — a regression check on the commit that moved the NVS
   write out from under the LVGL lock. **PASS.**
6. An existing `Win+L` macro now works **without being re-created** (the phantom-Shift fix,
   `9ed000a`). **PASS.**

**Do not over-read check 1.** It proves a long hold does not *break* the detent — it does not
prove the debounce fix's actual bug path was exercised. That bug needs the contact held **low**
for more than 768 ms (a `uint8_t` counter wrapping at 256 samples of a 3 ms poll), and earlier
measurement established that closure duration is set by the mechanical wipe of the contact, not by
how long the owner holds the knob: the longest closure ever captured was 588 ms, even during holds
the owner believed were much longer. So the debounce fix remains **correct by inspection** (the
saturating-counter arithmetic was re-verified by the final review) but **unexercised in practice**
— nothing has yet triggered the >768 ms path on real hardware, and check 1 passing does not change
that.

### Two findings — settled by measurement, do not re-litigate

- **The encoder is not a quadrature device.** Raw-pin capture (2026-08-13): rest state is
  `A=1 B=1`; one direction pulses `A` low while `B` never moves, the other direction pulses `B`
  low while `A` never moves. It is two independent momentary contacts, not a Gray-code encoder. A
  quadrature-decode rewrite was attempted (`b2a7aa3`) on the theory that "not currently decoded as
  quadrature" meant "should be" — it emitted zero events across a 90 s capture and was reverted
  (`3ada93d`). The vendor driver's edge-per-pin design is correct for this hardware. **Do not
  attempt a quadrature decode on this board again** without new electrical evidence.
- **~15% of detents emit two contact closures.** Measured: 29 of 189 same-direction inter-event
  gaps under 250 ms, cleanly separated from the 39 gaps in the 400–700 ms band that are genuine
  consecutive clicks. This is mechanical (two full closures ~240 ms apart is not healthy detent
  behaviour), not decodable in firmware — nothing in the signal distinguishes an intentional fast
  double-click from a bouncing switch. The owner has decided to live with it rather than add a
  lockout window, which would also cap deliberate fast turning. **If revisited, suspect the
  physical switch before suspecting firmware.**

---

## 5. Hardware — read before plugging anything in

The board is a **Waveshare ESP32-S3 knob**: ESP32-S3 rev v0.2, 16 MB quad flash, 8 MB PSRAM
(present but currently disabled), 360×360 AMOLED, CST816 touch, encoder on GPIO 8/7.
A **500 MB microSD card is installed** but the firmware does not use it (see §7).

### FQBN

```
esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled
```

Requires Espressif's core (`arduino-cli core install esp32:esp32`). The M5Stack core alone has no
suitable target. Every option choice is justified in the toolchain doc — in particular
`PartitionScheme=default_8MB` on a 16 MB board is **deliberate** (LittleFS needs a `spiffs`-subtype
partition; every 16 MB scheme on offer is FATFS), and switching it would silently break profile
persistence.

### Four quirks, each of which presents as a hardware fault

1. **USB-C plug orientation selects which MCU you reach.** The board has two — ESP32-S3R8 (ours)
   and ESP32-U4WDH — behind one port via a CH445P switch. Wrong way: VID `0x1A86`, esptool reports
   "ESP32" with 4 MB flash. Right way: VID `0x303A`. *The owner has marked the S3 side of the cable
   green.*
2. **Auto-reset does not work.** `esptool` failing with `No serial data received` means "hold BOOT
   and replug", not "board is broken".
3. **After uploading you must physically replug.** esptool's closing RTS hard-reset is a no-op; the
   board stays in download mode and runs nothing. Serial capture returns *complete silence*, which
   looks exactly like a boot loop. It isn't.
4. **Run vs download mode is told by `serialNumber`, not PID** (both are `0x303A:0x1001`).
   `FC012CD1DDD8` = running; empty = download mode. The COM port number also changes.

### Serial capture

Use `scripts/serial_capture.ps1 -Port <COM> -DurationSec <n> -LogPath <file>`.
**Do not use `arduino-cli monitor`** — it treats non-interactive stdin as an immediate quit, and
an earlier, now-fixed bug made a plain `.NET SerialPort` capture reset the board on *close*
(walking three of the four steps of the core's DTR/RTS bootloader-restart state machine, with the
fourth supplied by `Close()`). Fixed firmware-side by `Serial.enableReboot(false)`
(`Waveshare_LVGL_Test.ino:1067`) — don't remove it, and don't re-diagnose future silent-capture
gaps as "port contention" before checking the board's enumeration first.

### Companion app — build for Android, never Windows

```
cd companion_app
flutter build apk --debug     # ALWAYS build first -- `flutter install` does NOT rebuild
flutter install
```

`flutter doctor` reports a Visual Studio error. **Ignore it.** It only affects
`flutter run -d windows`, which this project never uses; the Android toolchain is green and the
APK builds in ~48 s. See `docs/FLUTTER_TOOLCHAIN_ISSUE.md` — a previous session lost time to a
proposed Visual Studio reinstall that would have fixed nothing.

---

## 6. What to do next, in order

### Step 1 — Final sign-off and merge

The outstanding hardware pass (§4) is complete — all six checks passed 2026-08-20 against
`9ed000a`.

**The debounce fix's unexercised >768 ms path (§4) was ruled non-blocking**, so that decision does
not need re-making. The fix saturates the counter at `DEBOUNCE_TICKS`, so the release edge's
pre-increment yields 3 and passes for every hold length; the counter can never exceed 3, which
means no wrap is reachable. That arithmetic was independently verified across three hold lengths
including 1000 polls during the final review. What it prevents is a dropped click — an annoyance,
not a hazard — and reproducing it requires parking the knob mid-detent to hold the contact low,
which costs a flash cycle for near-zero information. It is recorded as correct-by-inspection and
unexercised, and that is where it should stay unless the symptom is ever seen in the wild.

What remains is to triage the deferred-minor list in the ledger's final section — mostly cosmetic
(the `state_set_active_profile` sentinel, `update_profile_switch`'s pre-lock read, a stale
comment), none blocking — and hand off to `superpowers:finishing-a-development-branch`.

### Step 2 — M8b, then M9

M8b (uncap `pos`, key running-macro state by identity, bump `version: 3`) and M9 (icons on the
ring). The ring geometry M9 will draw icons into is now settled by the Task 8–10 rework, which is
exactly why M9 was sequenced after it rather than before. M9 should also settle the frame-pacing
question flagged in §4 — the first natural point at which a 12+ macro profile will actually get
loaded onto the device.

### Step 3 — M5Dial security gate

The M5Dial firmware still has no cryptographic gate at all (§7). This has been the top follow-up
since M6 closed the equivalent hole on the Waveshare board, and per the locked work order
(`CLAUDE.md`), Waveshare polish work should not keep displacing it indefinitely — it goes ahead of
M10's comfort items.

---

## 7. Hard constraints — do not violate

1. **No Wi-Fi, no HTTP server, no captive portal, no web UI.** Permanently cut. If a fix seems to
   need one, it is the wrong fix.
2. **No application-layer auth token.** M6 *removed* the `token`/`pair`/`pairingToken` scheme.
   Security is BLE pairing + bonding + GATT permission flags. Do not reintroduce it as a fallback
   — see the pairingToken rebuttal in §8, it has already been re-litigated once by an external
   audit and the removal held up under scrutiny.
3. **Threading rules** (spec §5, `CLAUDE.md`): macro engine is loop()-task only; UI callbacks use
   `macros_request_fire()`; LVGL only under `lvgl_lock()`; BLE callbacks hand off via queues; stop
   running macros (and drain the pending-fire queue — see §4) before a profile reload.
4. **Verify BLE and LVGL APIs against the installed core's headers, don't assume.** This build uses
   Bluedroid-styled class names but is **NimBLE-backed**, and the wrapper keeps the Bluedroid API
   surface while silently neutering it — `setAccessPermissions()` compiles and does nothing,
   `PROPERTY_*_ENC` is `0` on the Bluedroid branch, `addDescriptor(BLE2902)` is discarded. All fail
   *silently and insecurely*. This already caused two wrong prescriptions during M6, and this
   milestone's `LV_OBJ_FLAG_CLICKABLE` Critical (§4) is the same lesson applied to LVGL: a default
   assumed rather than read from source cost a working tap-to-enter.
5. **Do not claim hardware verification you did not perform.** State plainly what was observed and
   what was not. §4 exists because of this rule.

---

## 8. Open items and known gaps

*The detailed H1/H3 hardware verification logs and the Just Works rejection narrative that used
to live in this section are not lost — they're in this file's git history, in the version dated
2026-07-25. This rewrite compresses them because M6 is done; go there for the raw serial evidence.*

- **`TX CCCD` notify permission is still gated on encryption, not authentication — resolved, but
  conditionally.** `BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN` (`0x10000`) truncates away because
  `BLECharacteristic` stores properties in a `uint16_t`; that code has not changed. What the H1
  negative test and the Just Works rejection finding established is that this is *currently*
  unexploitable: across every pairing session captured, only `0/0/0` (declined) or `1/1/1`
  (completed) ever occurred — `authenticated=0 bonded=1`, the one state that would make the
  truncation matter, was never reachable, because `setAuthenticationMode()` uses an `SC_MITM_BOND`
  mode and the device rejects Just Works, escalating to a passkey prompt instead. **This finding is
  void the moment authentication mode stops requiring MITM.** If a future change drops or weakens
  the passkey prompt to make pairing friendlier, the CCCD becomes genuinely unprotected against
  Just Works with nothing flagging it, because the truncated bit was never actually enforcing
  anything — the MITM requirement upstream was.
- **M5Dial firmware has no cryptographic gate.** That sketch has no `BLESecurity` setup and no
  GATT permission flags at all — config access is gated solely on `CONFIG_MODE`. The second
  supported board still carries the vulnerability M6 exists to close. **Top follow-up**, now
  explicitly sequenced ahead of M10 (see §6 Step 5). Deferred by the board sequencing (spec §3),
  not by tooling — the hardware is on hand.
  > **Do not close this by restoring the `pairingToken`.** An external audit (2026-08) called the
  > removal a blocking regression; it was not. The token was checked *only outside* `CONFIG_MODE`,
  > and `CONFIG_MODE` is the only mode serving config commands — so in the mode that mattered
  > there was never a check. Accepted requests went from `{CONFIG_MODE: any} ∪ {RUN_MODE: token}`
  > to `{CONFIG_MODE: any}`: a strict subset, i.e. net *tightening*. The old `pair` command also
  > handed the token to any central in `CONFIG_MODE` and persisted it to NVS, converting one
  > moment of physical access into permanent remote `RUN_MODE` access. Full reasoning is in the
  > header comment of `firmware/M5_M6_config/M5_M6_config.ino`.
  > Verified against the real baseline: `origin/main:584` carries exactly that gate.
- **Haptics remain disabled** (breaks the CST816). A three-step single-variable re-enable plan is
  written at the call site in `Waveshare_LVGL_Test.ino`. Test by tapping *and* swiping — the
  encoder kept working right through the original failure, so it proves nothing.
- **PSRAM is disabled although 8 MB is present.** Kept out of the M6 stability diagnosis
  originally; that diagnosis is now settled (M6 is done), so enabling `PSRAM=opi` is a reasonable
  isolated experiment. Free heap has stayed flat all through M7/M8 (§4), which is workable but not
  generous, especially with M9's icon assets still to come.
- **500 MB microSD installed, unused.** Recommendation unchanged: keep `profiles.json` in LittleFS
  (internal, always present, no eject/corruption risk). The SD is a good home for **icon assets**
  at M9, which is the one thing likely to outgrow internal flash.
- **The app finds the device by scanning ONLY** (`draupnir_state.dart:248-287`) and never consults
  `FlutterBluePlus.systemDevices`. A peripheral does not advertise while a link is open, so if
  Android holds a stale ACL connection the scan returns nothing and the app reports "No Draupnir
  found" — presenting as *"it paired but the app won't connect."* Fix: check `systemDevices` first,
  fall back to scanning.
- **`_looksLikeAuthFailure`** in the companion app is an untested string heuristic against platform
  GATT error text.
- **`http` / `shared_preferences`** are unused in `companion_app/pubspec.yaml`.
- **H6's duplicate-write guard** (a 10 ms time-scoped guard, shipped as the interim fix instead of
  inbound sequence numbers because the same app also talks to the M5Dial firmware) may be
  unnecessary under NimBLE: it was written for Bluedroid's prepare/execute double-dispatch, and
  NimBLE calls `onWrite()` once per write. Check that on hardware before spending effort on real
  sequence numbers — if the guard is provably dead weight, deleting it is free.
- **Display controller discrepancy:** third-party sources describe the panel as ST77916; the
  firmware drives it with SH8601 and works. Unresolved, low priority.
- **`firmware/Waveshare_Knob_Config/`** is a superseded Adafruit_GFX port, still untracked, with
  leftover `refactor*.py` scripts. Safe to delete once nothing is owed to it.
- **Two long-lived branches exist and they are unrelated histories.** `main` is the real trunk.
  `origin/master` is a bare "Initial commit" that holds almost none of the tree, yet it is the
  repo's *default* branch (`origin/HEAD -> origin/master`), so tooling and fresh clones land on the
  empty one. Diffing against `master` gives a meaningless "all new" result. **Use `main`** (or, for
  this milestone's own history, `review/waveshare-m6-foundation`). Worth deleting or repointing
  `master` before it misleads anyone else.
