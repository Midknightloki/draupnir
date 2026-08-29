# Draupnir — Session Handoff

*Written 2026-08-22, updated 2026-08-29 for M8b. Pick up here.*

This is a continuation brief for whoever works on Draupnir next — another agent, a fresh session,
or the owner. It assumes **no prior context**.

---

## 1. Read these first

| Document | What it gives you |
|---|---|
| `CLAUDE.md` / `AGENTS.md` | Persona, locked decisions, threading rules, FQBNs. Identical copies — **edit both together.** |
| `docs/Draupnir_Spec.md` (v3) | The brief. Concept, hardware, data model, BLE protocol, milestones (§10 has the current state). |
| `docs/superpowers/specs/2026-08-26-m8b-uncap-pos-design.md` | The design the **most recent** milestone (M8b) was built against. |
| `.superpowers/sdd/2026-08-26-m8b-uncap-pos/progress.md` | M8b's execution ledger, including its hardware verification and the BLE-naming follow-on. |
| `docs/superpowers/specs/2026-08-20-m9-icons-orientation-rotary-design.md` | The design M9 was built against. |
| `docs/superpowers/plans/2026-08-20-m9-icons-orientation-rotary.md` | M9's task-by-task implementation plan. |
| `.superpowers/sdd/2026-08-20-m9-icons-orientation-rotary/progress.md` | The full execution ledger for M9 — every task, review finding, hardware round, and owner decision, in order. §3–§4 below summarise it; the ledger is the source of truth. |
| `.superpowers/sdd/2026-08-07-m7-m8-persistence/progress.md` | The equivalent ledger for the previous milestone (M7/M8), still relevant background. |
| `docs/Toolchain_arduino-cli.md` | **Read before touching hardware.** Board quirks below will otherwise cost you hours. |

---

## 2. The goal

Draupnir is a **USB-HID macro controller in a knob** — round touch screen plus rotary encoder,
driverless over USB HID, configured from a Flutter phone app over BLE. Macros show as a ring of
colored wedges; rotate to select, tap center or tap a wedge to fire.

**M6 (config hardening), M7 (NVS persistence), M8 (on-device profile switching), M9 (icons,
dial orientation, rotary macro mode) and M8b (uncap `pos`) are done**, verified on hardware. M9's
Waveshare work was signed off 2026-08-22 after three feedback rounds; the M5Dial's `icon_xbm`
save-merge — a fix for live data loss on that board — was flashed and verified 2026-08-25. Editing
one macro no longer wipes the icons on the others. M8b was verified on both boards 2026-08-29
against the full criteria list, closing the last gap between the schema and what the spec has
claimed since v3. See §4.

---

## 3. Where things stand

Branch **`feat/m8b-uncap-pos`**, 9 commits ahead of `feat/m9-icons-orientation-rotary`, which it
branches from. `review/waveshare-m6-foundation` is the integration branch every milestone PRs
into: PR #2 (M7/M8) and PR #3 (M9) are merged there, PR #4 (M9's M5Dial verification docs) is
still open, and M8b stacks on top of it.

```
295dc5a  docs: M8b design — uncap pos, schema v3
ed13255  docs: M8b implementation plan — six tasks
9ba1750  feat(m8b): key running-macro state by identity, not by pos
2cffbcc  feat(m8b): check the schema version instead of just declaring it
59fc5f3  feat(m8b): uncap the M5Dial macro engine, keep its 16-dot ring
17a98b0  feat(m8b): deck renders what exists, plus a trailing + tile
60ee198  docs: M8b — replace the NOT YET IMPLEMENTED box with what shipped
b4d7583  feat: distinct board names, a picker, and a real Config Mode message
e559c21  docs: M8b verified on hardware — promote it, re-point the handoff   <- HEAD
```

The cap was never `NUM_MACRO_SLOTS`; it was that `runningMacros[]` was indexed **by `pos`**, so
raising the constant would have compiled and been the wrong fix. The pool is now keyed by macro
identity: `MAX_MACROS` 32 addressable, `RUNNING_SLOTS` 16 concurrent, and firing a 17th concurrent
macro is refused and logged rather than corrupting a slot. `b4d7583` is an unplanned follow-on —
see §4 and the M8b ledger.

Below is the M9 branch state, still accurate for the branch under it:

```
7a29c90  refactor(ui): dispatch input through a mode table
a65d8a8  docs(ui): restore comments dropped in the mode-table refactor
2387c33  feat(m9): consume settings.orientation via MADCTL
cd1a353  feat(m9): render macro icons on the ring
94ca06e  fix(m9): add icon drop shadow, refactor contrast into bg_is_light()
417546b  feat(m9): implement rotary macro mode
570285a  fix(m9): rotary exit must not kill unrelated macros
f53206f  feat(m9): strip icon_xbm on read, merge it on save
4d182e2  fix(m5): merge preserved icon_xbm on save
bd9b358  fix: rotary/settings idle-timeout tap leak + per-detent rotary firing
3ec0a1a  fix: four M9 hardware-testing bugs — MADCTL opcode, icon zoom, gesture reset, encoder judder
d04a90c  fix: three M9 hardware round-2 bugs — icon zoom, swipe-as-tap, spin lag
0b84066  fix: retarget macro icon scaling to 2.5x via inverse-mapping resample
e4b6db8  feat: add orbitron_18 font, bump wedge labels to 18px    <- HEAD
```

Tasks 1–6 of the M9 plan are each individually review-clean (see the ledger for the finding trail
on each). A final whole-branch review then caught two cross-task defects invisible to any
single-task review — a Critical UI-mode guard that could route an exit-tap into firing a macro, and
an Important rotary multi-detent coalescing bug — both fixed in `bd9b358` and re-reviewed clean.
Everything from `3ec0a1a` onward is a hardware-round fix (see §4). None of those four commits went
through a task brief or a reviewer at the time — they were written live during hardware debugging,
when the feedback loop was the bottleneck rather than the code. That debt was cleared before the
PR: all four were reviewed together against the full diff, returning **0 Critical, 1 Important,
7 Minor**. The two worth worrying about — the icon scaler's bounds arithmetic and
`select_idx_by()`'s task affinity — were checked against the installed LVGL 8.4.0 sources and the
vendored SH8601 driver, and are correct. Three risk-free items were fixed (`ef72c3e`); the rest are
parked with reasoning in
`.superpowers/sdd/2026-08-20-m9-icons-orientation-rotary/hw-rounds-review.md`.

Worth recording that skipping the gate *was* a gamble that happened to pay, not a process this
project validated. It was defensible under live hardware debugging and should not become the
default.

---

## 4. Verified vs. not verified on hardware

**This is the most important section in this document.** Do not build on anything listed as "not
verified" as though it were confirmed.

### Verified on hardware during M8b — both boards, 2026-08-29

Flashed Waveshare COM10 and M5Dial COM5 on 2026-08-28, both hashes verified, then walked the full
criteria list in `docs/Draupnir_Spec.md` §7 in the mandated order:

- **No regression, run first and on both boards** — existing profiles load, render, fire and stop,
  toggles included. This was the gate: the pool rewrite touches every fire, stop and query path in
  the product, so a regression here would have invalidated everything after it.
- **Wedge ordering unchanged**, including across a save — `scan_active_positions()` now sorts
  ascending by `pos` explicitly rather than inheriting JSON array order.
- **A macro above `pos` 15** renders, selects and fires on the Waveshare; **deleting a middle
  macro** reflows the ring with the remainder still in ascending order.
- **A 17th concurrent macro is refused and logged**, with the 16 already running unharmed, and
  **kill-all stops a macro above `pos` 15**.
- **A `"version": 4` file is refused** without booting to an empty ring.
- **The M5Dial divergence behaves as documented** — a macro above `pos` 15 fires but does not
  appear on that board's 16-dot ring. Expected, not a bug; see spec §6.

### NOT verified from the M8b follow-on (`b4d7583`)

Distinct board names, the multi-device picker, and the Config Mode panel were **flashed to the
M5Dial 2026-08-29 but never exercised.** The firmware half is one string; the two halves that
matter — the picker and the Config Mode state — are app-side, and the APK was not installed. Build
evidence only: M5Dial compile clean at 1,643,251 bytes (49%), `flutter analyze` 0 errors, debug APK
built. Treat all of it as unverified.

### Verified on hardware during M9 (Waveshare)

- **The mode-dispatch refactor** (`7a29c90`), behavior unchanged — confirmed across all the
  hardware rounds below, which exercised ring, settings and rotary modes repeatedly with no
  regression traceable to the refactor itself.
- **Dial orientation**, all four values (`0`/`1`/`2`/`3`), in **both** the display (MADCTL write)
  and touch (coordinate transform). Round 1 found orientation doing nothing visually while touch
  still rotated — the two are independent code paths and can be right/wrong independently, which is
  exactly what happened. Round 2 confirmed the display now rotates correctly for all four values
  after the QSPI opcode fix (see §8 finding 1).
- **Macro icons at 45×45**, luminance-picked black-or-white plus an opposite-colour 1px drop
  shadow. This took three sizing rounds on hardware: 18×18 (the app/M5Dial interchange size) was
  far too small on the 360×360 panel, 3× zoom made icons vanish entirely (see §8 finding 2), the
  fallback pre-scale to 54×54 worked but was "slightly large," and the final 45×45 (2.5×,
  inverse-mapping resample) was accepted — "pixelized look I didn't intend, but I don't hate it."
- **Rotary macro mode** — "works great" (owner, round 1). Entering does not stop macros; exiting
  stops only the rotary binding, matching the M5Dial's behavior; per-detent firing was confirmed
  after the multi-detent coalescing fix (`bd9b358`).
- **`icon_xbm` strip-and-merge on the Waveshare** — editing one macro preserves every other macro's
  icon, confirmed on hardware (round 1: "editing macros/icons preserves other macros' icons").
- **Wedge labels at 18px** (`orbitron_18`), confirmed together with the 45px icons in the final
  hardware round: "This looks good, I think we can mark M9 complete." (owner, 2026-08-22).
- Swipe gestures, after two rounds of tuning (`gesture_min_velocity` 1→0, then `gesture_limit`
  40→25) — "much better," then working at both fast and slow swipe speeds.
- Fast encoder spin tracking the target angle without freezing or reversing, after two rounds
  (`select_idx_by()`, then a bounded ease lag) — confirmed smooth in round 2.
- **The M5Dial's `icon_xbm` save-merge (2026-08-25).** Flashed to the M5Dial and verified against
  its stated criterion: edit one macro in the app, the other macros keep their icons. This closed
  live data loss — before it, every save wiped the bitmaps on every macro the user had not just
  edited. Note what a pass does *not* cover: that board's `profiles.json` write is still a direct
  truncate-and-write, so power loss mid-save still corrupts it (see §6).

### NOT verified — read this before assuming otherwise

- **Frame pacing at high macro counts.** Still unmeasured — all M9 hardware testing ran against
  small profiles. This is now a harder question than it was at the end of M7/M8: the ring draw
  callback issues up to 9 `lv_draw_label` calls per wedge per repaint (unchanged from before), and
  each wedge now also draws a 45×45 icon plus its drop shadow, in the same nine-outline-copy
  pattern. Nobody has loaded a 12+ macro profile onto the device since M7/M8, let alone with M9's
  larger assets.
- Swipe-down killing macros from the rotary screen — implemented, not specifically hardware-tested
  this milestone.
- 180°/270° touch accuracy in isolation — orientation was reported working as a whole, but round 1
  showed display and touch can be right/wrong independently of each other, so a value-by-value
  breakdown was never isolated; only the aggregate "rotates for all four values" was confirmed.

### Four findings — do not re-derive these on the next hardware round

Each of these cost a full hardware round (flash → observe → diagnose → fix → reflash) to find. All
four are instances of code that compiles cleanly and does nothing observable — treat that symptom
as the first hypothesis, not a last resort (see the tally below).

1. **The SH8601 is a QSPI panel; commands must carry the write opcode.** `lcd_set_orientation()`
   sent a raw `0x36` MADCTL byte, which the panel silently ignored, while the touch transform (plain
   C, no panel involvement) worked regardless — hence "touch rotates, display does not." The
   driver's own command path wraps every byte as `(cmd & 0xff) << 8 | (0x02 << 24)`; fixed with a
   `SH8601_QSPI_CMD()` macro replicating that encoding.
2. **LVGL 8.4 cannot zoom an `ALPHA_1BIT` image.** `lv_draw_sw_img.c` takes the transform path
   whenever `zoom != LV_IMG_ZOOM_NONE`, and `lv_draw_sw_transform.c` only implements
   `TRUE_COLOR`/`TRUE_COLOR_ALPHA`/`TRUE_COLOR_CHROMA_KEYED`/`RGB565A8`. A 1-bit source under `zoom`
   renders nothing — not small, not distorted, absent. Fixed by pre-scaling the bitmap in software
   (nearest-neighbour, later switched to inverse-mapping to support the non-integer 2.5× ratio) and
   drawing at 1:1 on the supported path.
3. **`gesture_min_velocity` must be 0, not 1.** LVGL resets accumulated gesture travel whenever
   `|vect| < min_velocity` on both axes. At 1, that condition is true on every poll where the finger
   moved zero pixels between samples — which is constant during a slow, deliberate swipe at a 3 ms
   poll rate. The reset then fires the whole way through the swipe, so the eventual release lands as
   a tap instead, firing whatever wedge is under the finger (including, once, a rotary macro the
   owner was only swiping past). A previous milestone had already moved this 3→1 and stopped one
   short of the value that disables the reset entirely.
4. **The wedge label font is referenced in four places and they must change together.** Three are
   inside `wedge_label_fit()` (the fits-as-is check, the ellipsis width, the truncation loop); the
   fourth is the draw call itself. `lv_draw_label` does not clip to its `coords`, so a
   measure/draw font mismatch makes text overflow its box and paint across the neighbouring wedge —
   in all nine outline-drawing copies. This exact failure class was fixed once already in an earlier
   milestone for a different reason; the M9 font swap (`orbitron_12` → `orbitron_18`) had to
   re-satisfy the same four-site invariant, verified by grepping the old font down to 0 references
   afterward.

**Tally worth keeping in view:** seven LVGL/esp_lcd APIs have now compiled cleanly and done nothing
in this project's history, two of them (findings 1 and 2 above) in M9 alone. When something "should
work" and visibly doesn't, absence-not-error is the pattern to suspect first here.

---

## 5. Hardware — read before plugging anything in

The board is a **Waveshare ESP32-S3 knob**: ESP32-S3 rev v0.2, 16 MB quad flash, 8 MB PSRAM
(present but currently disabled), 360×360 AMOLED, CST816 touch, encoder on GPIO 8/7.
A **500 MB microSD card is installed** but the firmware does not use it (see §8).

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

### Step 0 — verify the M8b follow-on, or explicitly drop it

`b4d7583` shipped unverified (§4). Install the debug APK, power both boards, and confirm three
things: the picker appears and both boards are distinguishable in it, picking `Draupnir_Mini` in
Run Mode produces the CONFIG MODE REQUIRED panel rather than a connection error, and TRY AGAIN
after swiping down loads profiles over the existing link without a rescan. Cheap, and it is the
only unverified thing on the branch.

### Step 1 — The M5Dial security gate

The M5Dial firmware still has no cryptographic gate at all (§8). This has been the top follow-up
since M6 closed the equivalent hole on the Waveshare, and per the locked work order (`CLAUDE.md`),
Waveshare polish should not keep displacing it — it goes ahead of M10's comfort items. It is an
unauthenticated keystroke-injection path on a currently-supported board: any BLE central in range
can write macros to it and have them typed into whatever host it is plugged into.

Fold the non-atomic `profiles.json` write (§6 gaps) into the same pass. Both are the same shape of
debt — hardening the Waveshare already received and the M5Dial never did — and both touch that
sketch's config path, so doing them together costs one review cycle instead of two.

### Step 2 — M10, polish

Buzzer/haptic feedback and export/import. Note that haptics are currently blocked by a hardware
interaction, not by effort: `haptics_init()` breaks the CST816 touch controller (§6 gaps). That
needs diagnosing before the milestone can be scoped honestly.

### Also worth doing, not blocking

- **Frame-pacing measurement at 12+ macros** (§4). This is the oldest unmeasured assumption in the
  project and it got worse in M9, not better. It also gates a known optimisation: `icon_scale()`
  re-derives every icon on every repaint for a result that only changes on profile reload, and the
  fix is a ~4.3 KB cache — but nobody should spend RAM on a problem nobody has measured.
- The five parked Minor findings from the M9 hardware-round review, recorded with reasoning in
  `.superpowers/sdd/2026-08-20-m9-icons-orientation-rotary/hw-rounds-review.md`.

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
   running macros (and drain the pending-fire queue) before a profile reload.
4. **Verify BLE and LVGL APIs against the installed core's headers, don't assume.** This build uses
   Bluedroid-styled class names but is **NimBLE-backed**, and the wrapper keeps the Bluedroid API
   surface while silently neutering it — `setAccessPermissions()` compiles and does nothing,
   `PROPERTY_*_ENC` is `0` on the Bluedroid branch, `addDescriptor(BLE2902)` is discarded. All fail
   *silently and insecurely*. M9 turned up two more instances of the same lesson applied to
   LVGL/esp_lcd (§4's findings 1 and 2) — a default assumed rather than read from source, twice,
   in the same milestone.
5. **Do not claim hardware verification you did not perform.** State plainly what was observed and
   what was not. §4 exists because of this rule.

---

## 8. Open items and known gaps

*The detailed H1/H3 hardware verification logs and the Just Works rejection narrative that used
to live in this section are not lost — they're in this file's git history, in the version dated
2026-07-25. This rewrite compresses them because M6 is done; go there for the raw serial evidence.*

- **M5Dial firmware has no cryptographic gate.** That sketch has no `BLESecurity` setup and no
  GATT permission flags at all — config access is gated solely on `CONFIG_MODE`. The second
  supported board still carries the vulnerability M6 exists to close. **Top follow-up**, sequenced
  ahead of M10 (see §6 Step 1). Deferred by the board sequencing (spec §3), not by tooling — the
  hardware is on hand.
  > **Do not close this by restoring the `pairingToken`.** An external audit (2026-08) called the
  > removal a blocking regression; it was not. The token was checked *only outside* `CONFIG_MODE`,
  > and `CONFIG_MODE` is the only mode serving config commands — so in the mode that mattered
  > there was never a check. Accepted requests went from `{CONFIG_MODE: any} ∪ {RUN_MODE: token}`
  > to `{CONFIG_MODE: any}`: a strict subset, i.e. net *tightening*. The old `pair` command also
  > handed the token to any central in `CONFIG_MODE` and persisted it to NVS, converting one
  > moment of physical access into permanent remote `RUN_MODE` access. Full reasoning is in the
  > header comment of `firmware/M5_M6_config/M5_M6_config.ino`.
  > Verified against the real baseline: `origin/main:584` carries exactly that gate.
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
- **Review-gate debt (M9).** Four commits went straight from a hardware finding to implementation
  with no task brief and no reviewer: the two hardware-round fix waves (`3ec0a1a`, `d04a90c`) and
  the two later icon/label sizing changes (`0b84066`, `e4b6db8`). They are substantive — the QSPI
  opcode fix and the icon scaler are among them — and a reviewer should see them before this branch
  merges. Same handling as the M7/M8 milestone's own hardware-round commits, which carry the same
  debt and haven't been retroactively reviewed either.
- **Frame pacing at 12+ macro profiles.** Unmeasured through both M7/M8 and M9. The ring draw
  callback issues up to 9 `lv_draw_label` calls per wedge per repaint, and now also draws a 45×45
  icon plus drop shadow per wedge in the same pattern; `EXAMPLE_LVGL_BUF_HEIGHT` is `V_RES / 10`, so
  the callback runs once per render stripe, not once per frame. Documented fallback is dropping to
  4 label offsets if it stutters. Nobody has loaded a 12+ macro profile onto the device yet.
- **fireQueue depth 8 vs. the rotary mode's multiplicative enqueue.** `encoder_task` coalesces from
  a 32-deep raw queue; a spin fast enough to queue more than 8 detents before the fire-queue drains
  will silently drop the excess. Parked deliberately (documented tradeoff, human spins rarely exceed
  a handful of detents per wake, `macros_update()` drains to empty every loop tick). Testable on
  hardware: if a fast spin during a rotary macro visibly drops detents, the fix is
  `xQueueCreate(8 → 16)`, one character.
- **`macros_request_rotary_step()`'s `-dir` is UB on `INT_MIN`** in the abstract. Parked: `dir`
  comes only from a coalesced sum of `int8_t` ticks off a 32-deep queue, bounded near ±4064 — the
  path is structurally unreachable.
- **The encoder is not a quadrature device.** Raw-pin capture (2026-08-13): rest state is
  `A=1 B=1`; one direction pulses `A` low while `B` never moves, the other direction pulses `B`
  low while `A` never moves. It is two independent momentary contacts, not a Gray-code encoder. A
  quadrature-decode rewrite was attempted on the theory that "not currently decoded as quadrature"
  meant "should be" — it emitted zero events across a 90 s capture and was reverted. The vendor
  driver's edge-per-pin design is correct for this hardware. **Do not attempt a quadrature decode on
  this board again** without new electrical evidence.
- **~15% of detents emit two contact closures.** Measured: 29 of 189 same-direction inter-event
  gaps under 250 ms, cleanly separated from the 39 gaps in the 400–700 ms band that are genuine
  consecutive clicks. This is mechanical (two full closures ~240 ms apart is not healthy detent
  behavior), not decodable in firmware — nothing in the signal distinguishes an intentional fast
  double-click from a bouncing switch. The owner has decided to live with it rather than add a
  lockout window, which would also cap deliberate fast turning. This is also why encoder detent
  alignment was dropped from M9's scope (spec §10) rather than delivered — the M7/M8 ring rework
  already centres every selection at 12 o'clock, so there was no alignment left to do; only this
  mechanical double-step remains, and it isn't a firmware problem. **If revisited, suspect the
  physical switch before suspecting firmware.**
- **Haptics remain disabled** (breaks the CST816). A three-step single-variable re-enable plan is
  written at the call site in `Waveshare_LVGL_Test.ino`. Test by tapping *and* swiping — the
  encoder kept working right through the original failure, so it proves nothing.
- **PSRAM is disabled although 8 MB is present.** Free heap has stayed flat through M7/M8/M9, which
  is workable but not generous, especially now that M9 added icon assets and a larger font. Enabling
  `PSRAM=opi` remains a reasonable isolated experiment.
- **500 MB microSD installed, unused.** Recommendation unchanged: keep `profiles.json` in LittleFS
  (internal, always present, no eject/corruption risk).
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
- **The M5Dial's `profiles.json` write is non-atomic.** Direct `LittleFS.open(..., "w")`
  truncate-and-write, unlike the Waveshare's temp→verify→rename (the H4 hardening). A power loss
  mid-save corrupts the config on the M5Dial and cannot on the Waveshare. Not user-visible in normal
  operation, so not a parity break M9 needed to fix, but it's a real robustness divergence between
  two supported boards — belongs in the M5Dial catch-up pass alongside the security gate.
- **Two long-lived branches exist and they are unrelated histories.** `main` is the real trunk.
  `origin/master` is a bare "Initial commit" that holds almost none of the tree, yet it is the
  repo's *default* branch (`origin/HEAD -> origin/master`), so tooling and fresh clones land on the
  empty one. Diffing against `master` gives a meaningless "all new" result. **Use `main`** (or, for
  this milestone's own history, `feat/m9-icons-orientation-rotary` / `feat/m7-m8-persistence`).
  Worth deleting or repointing `master` before it misleads anyone else.
