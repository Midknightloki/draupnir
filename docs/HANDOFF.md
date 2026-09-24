# Draupnir — Session Handoff

*Written 2026-08-22, updated 2026-09-09. Pick up here.*

This is a continuation brief for whoever works on Draupnir next — another agent, a fresh session,
or the owner. It assumes **no prior context**.

---

## 1. Read these first

| Document | What it gives you |
|---|---|
| `CLAUDE.md` / `AGENTS.md` | Persona, locked decisions, threading rules, FQBNs. Identical copies — **edit both together.** |
| `docs/Draupnir_Spec.md` (v3) | The brief. Concept, hardware, data model, BLE protocol, milestones (§10 has the current state). |
| `docs/superpowers/specs/2026-09-07-profile-export-import-design.md` | The design the **most recent** work (profile export/import) was built against. |
| `docs/superpowers/plans/2026-09-07-profile-export-import.md` | Its task-by-task implementation plan. |
| `docs/superpowers/specs/2026-09-03-m5dial-security-gate-design.md` | The design the M5Dial security gate was built against. |
| `docs/superpowers/plans/2026-09-03-m5dial-security-gate.md` | Its task-by-task implementation plan. |
| `docs/superpowers/specs/2026-08-26-m8b-uncap-pos-design.md` | The design the previous milestone (M8b) was built against. |
| `.superpowers/sdd/2026-08-26-m8b-uncap-pos/progress.md` | M8b's execution ledger, including its hardware verification and the BLE-naming follow-on. |
| `docs/superpowers/specs/2026-08-20-m9-icons-orientation-rotary-design.md` | The design M9 was built against. |
| `docs/superpowers/plans/2026-08-20-m9-icons-orientation-rotary.md` | M9's task-by-task implementation plan. |
| `.superpowers/sdd/2026-08-20-m9-icons-orientation-rotary/progress.md` | The full execution ledger for M9 — every task, review finding, hardware round, and owner decision, in order. §3–§4 below summarise it; the ledger is the source of truth. |
| `.superpowers/sdd/2026-08-07-m7-m8-persistence/progress.md` | The equivalent ledger for the previous milestone (M7/M8), still relevant background. |
| `docs/Toolchain_arduino-cli.md` | **Read before touching hardware.** Board quirks below will otherwise cost you hours. |
| `docs/Waveshare_Hardware_Reference.md` | **Pinout, transcribed from the manufacturer's schematic** (archived in `docs/hardware/`). Answers what is wired where, and corrects three things this repo previously had wrong. |

---

## 2. The goal

> **Board status changed 2026-09-23.** The **Waveshare ESP32-S3 knob is the only board that
> ships.** The M5Dial is **officially retired** — not sold, not supported, no further work — and
> **frozen for the owner's personal use**: it still builds, runs, and reads the current schema.
> Because a frozen device receives no firmware updates, **every shared-layer change (schema, BLE
> protocol, macro engine, export format) must be additive.** Dated entries below that describe the
> M5Dial as a supported second target were accurate when written; read them as history.

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

**The M5Dial security gate is also done**, verified on hardware 2026-09-06. That board now pairs,
bonds and enforces GATT permission flags like the Waveshare, its `profiles.json` write is atomic,
and its Wi-Fi stack and LAN-reachable web API — a remote keystroke-injection path on a device that
is a keyboard — are deleted outright. Both supported boards are now on the same security posture,
and both are proven by **refusal**, not merely by acceptance: the hostile-central test passed on
the Waveshare 2026-08-07 and on the M5Dial 2026-09-07. Nothing security-related is outstanding.

**Profile export/import is done and fully verified** — an enveloped JSON file, whole-config
restore, single-profile sharing via the OS share sheet, and an undo that survives an app restart.
Verified on the M5Dial 2026-09-08, and the **cross-device** transfer — the criterion a same-device
round trip structurally cannot prove — passed on both boards 2026-09-09.

**The app is rebranded as Draupnir Forge** (logo, launcher icon, brand palette), in PR #12.

**The haptics/touch conflict is solved** (2026-09-09): the cause was a diagnostic I²C bus scan, not
haptics. See §4 finding 6. Haptics is enabled, and the firmware side is verified correct by
register readback — but **the motor on this unit does not respond**, because the DRV2605 reports
its output faulted with over-current. That is a hardware fault, not a firmware one, and it is
accepted as a nice-to-have gap rather than a blocker. Listed under NOT verified.

---

## 3. Where things stand

**Everything through export/import is merged to `master`.** The stacked-branch era is over: the
five feature branches and the `review/waveshare-m6-foundation` integration branch are all merged
and deleted, and `main` (a stale M6-era branch that had neither the security gate nor
export/import) is gone. `master` is the default and the only long-lived branch.

**PR #12 (the Draupnir Forge rebrand) is merged.** `master` carries the branding, the logo and
launcher-icon assets, and `design/generate_app_icons.py`, which regenerates every asset from the
tracked master logo.

A caution earned the hard way. Four PRs once all showed "merged" while `master` had **neither** the
security gate nor export/import, because each had merged into its own stacked base and nothing
propagated upward. `master` sat for a while missing the milestone that closed a remote
keystroke-injection hole. **PR into `master` directly**; do not rebuild the stack.

Below is the M9 branch state, kept for the commit-level history it records:

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

### Verified from the M8b follow-on (`b4d7583`) — 2026-08-29

Distinct board names, the multi-device picker, and the Config Mode panel: flashed to the M5Dial,
APK installed, and exercised end to end. The app distinguishes the two boards, connects to the one
you pick, and functionality was validated on the **Draupnir_Mini** specifically.

One word of care in how this gets recorded: what was exercised was a **connect**, not a pair. The
M5Dial has no BLE security, so there is nothing to pair with on it — any PIN dialog seen during
testing came from the Waveshare. This does not weaken the result; it is the hole the next milestone
closes.

> *Historical, accurate as of 2026-08-29. That hole is now closed — see "M5Dial security gate"
> below. The M5Dial pairs, bonds and enforces on its own as of 2026-09-06.*

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

### Verified on hardware — M5Dial security gate, 2026-09-06

Flashed the M5Dial (COM5 download / COM7 run), hash verified, and walked the criteria with a
direct `SerialPort` capture rather than by eye. All seven pass.

- **Wi-Fi and the web server are gone.** `draupnir.local` does not resolve. Flash dropped
  1,643,251 → 966,811 bytes (−676 KB) on the deletion commit — you cannot leave `WebServer`
  accidentally linked in and lose 660 KB, so the size is itself evidence the removal was complete.
- **BLE re-advertises after a disconnect**, the trap in that deletion (`startAdvertising()` lived
  inside the Wi-Fi restore block): `Disconnected` → `Restart BLE advertising` → `Connected`, all
  within three seconds, no reboot.
- **Pairing works.** Phone prompts, a 6-digit passkey appears on the dial, entering it completes:
  `authentication complete, encrypted=1 authenticated=1 bonded=1`.
- **Reconnect is silent** — bond reused, no second passkey.
- **The gate authorizes, and Config Mode no longer does.** Read *and* save succeed with the dial
  in **Run Mode**. Across the session: **5 commands accepted, every one logging
  `enc=1 auth=1 bond=1`; zero refused**. `trigger` — the command that actually types into the host
  — is among them.
- **The atomic write commits and reloads:** `save_profiles: committed 12597 bytes`, then 12650 on
  a second save, each followed by a clean `Loaded profiles.json`. No `write failed`, no
  `rename into place failed`. **The edit survived a power cycle.**
- **The hostile-central (negative) test passes — confirmed 2026-09-07.** An unbonded central is
  refused. This is the criterion that separates "the gate accepts authorized traffic" from "the
  gate refuses unauthorized traffic", and until it was run the security-gate round proved only the
  former. It was recorded as NOT verified for a day rather than folded into the positive results,
  which is the right way round: the Waveshare's equivalent claim (2026-08-07) had always been
  backed by a real session, and the two should never have read as equivalent before this.
- **Pairing while sitting in Config Mode** returns to the Config Mode screen cleanly rather than
  stranding the passkey screen. This needed a fix found by reading rather than testing:
  `requestRedraw()` is only ever dispatched inside the `RUN_MODE` branch of `loop()`, so the
  end-of-pairing repaint was a no-op in Config Mode. `drawConfigModeScreen()` was split out of
  `enterConfigMode()` so that path can repaint directly, without re-running the latter's
  `killAllMacros()` side effect.

**A bug this round found and fixed, worth knowing about:** the RX duplicate-write guard had no
expiry, so it compared against the previous write *forever*. The app's first command after a
reconnect is byte-identical to its first command last session, so the device bonded and
reconnected perfectly and then **silently swallowed every command** — presenting as "cannot
reconnect, and restarting the app doesn't help", since the stale state is on the device and only a
reboot cleared it. Pre-existing, not introduced by the gate; it was simply unreachable until
someone reconnected the M5Dial twice in one power cycle. Fixed by porting the Waveshare's 10 ms
window (`1c7e3b8`).

### Verified on hardware — profile export/import, 2026-09-08

M5Dial flashed (hash verified) and the debug APK installed on a Pixel 10 Pro, with a direct
`SerialPort` capture alongside. The exported files were validated by parsing them, not by eye.

- **No regression on the path every fetch uses** — a normal `get_profiles` still strips icons:
  `streamed bytes = 12420 (icons=0)`. This was the gate.
- **`include_icons: true` works end to end** — `streamed bytes = 12678 (icons=1)`, and the
  arithmetic reconciles exactly: 258 bytes more than the stripped fetch, which is two icons at 122
  bytes (`,"icon_xbm":"` + 108 hex + `"`) plus one macro carrying an *empty* `icon_xbm` at 14. The
  passthrough is byte-exact, not approximately right.
- **The exported file is complete and correct.** 41,154 bytes, parses cleanly — so the bypass
  path's single-flush is right and the tail is not truncated. Envelope carries
  `draupnir: "config"`, `schema: 2`, `exported`, `payload`; 3 profiles, 4 settings keys, 24 macros,
  **2 with real `icon_xbm` bitmaps**.
- **`schema` carries the source document's own version.** The file says `schema: 2` and
  `payload.version: 2`. Writing `3` (the app's maximum) would have mislabelled a v2 document in a
  way the importer could not detect.
- **Import restores**, with icons still rendering on the ring, and `save_profiles: committed 12650
  bytes` on the device.
- **Single-profile export** produces a valid `draupnir: "profile"` envelope (2,483 bytes).
- **A bad `schema` is refused** by the app before anything is sent to the device.
- **Undo works and survives an app restart** — the `shared_preferences` snapshot outlives the
  process, which an in-memory undo would not.
- **Abrupt client disappearance is handled.** Force-closing the app mid-transfer produced
  `send aborted (chunk ack retries exhausted)` followed by a clean re-advertise and reconnect at
  `encrypted=1 authenticated=1 bonded=1`. Correct recovery, not a fault.

**A design call this round vindicated:** the owner's live config contains **6 macros with no `pos`
field at all**. The spec deliberately forbade an app-side "every macro must have a `pos`" check
because the firmware tolerates its absence. Had that seemingly obvious validation been added, the
owner's own config would have been rejected on import.

**The bug this round found, and what hid it.** Export silently did nothing at first:
`file_selector_android 0.5.2+8` overrides only `openFile`, `openFiles` and `getDirectoryPath`, so
`getSaveLocation()` falls through to the platform-interface default, which **throws
`UnimplementedError`** — into an async gap with no UI. The asymmetry is the lesson: **import uses
`openFile`, which IS implemented, so watching import work said nothing about export.** Fixed by
writing to `Directory.systemTemp` and handing the file to the OS share sheet (`share_plus`), which
also serves the "share a profile with someone else" purpose better than a save dialog would. Both
export and import now surface failures in a dialog — the original defect was not merely the wrong
API, it was the wrong API failing invisibly.

### Verified on hardware — 2026-09-09, both boards

Both boards on current firmware for the first time (the Waveshare had only ever been *compiled*
against the export/import work until tonight).

- **The cross-device transfer passes.** A profile exported from the M5Dial and imported to the
  Waveshare arrives with its custom `icon_xbm` bitmaps rendering on the ring, with no editing.
  This is the criterion a same-device round trip **structurally cannot** prove — `save_profiles`
  merges stored bitmaps back by `pos`, so the device commits an identical document whether or not
  the file carried any. Export/import is now fully verified.
  > **Use a profile that actually has bitmaps.** The first attempt used `Windows controls`, which
  > has icon *names* but zero `icon_xbm`, so nothing icon-shaped was in the file and the absence
  > looked like a bug. `Autofill` (`Pro`, `Lock`) is the only profile in the owner's config with
  > real bitmaps. This cost a round; check the file before concluding anything.
- **The release build works.** `flutter build apk --release` and `appbundle` both succeed, and
  BLE connects and fetches profiles from the release APK on a Pixel 10 Pro. R8 minification does
  **not** strip anything `flutter_blue_plus` needs — which was the open risk, since the failure
  mode would have been a silent "scan finds nothing" reaching users rather than a build error.
  Size is a non-issue: 50.4 MB of the 52.8 MB APK is native libs for three ABIs, and Play splits
  an AAB per device (~19 MB on arm64). Do not "optimise" it.
- **The haptics/touch conflict is root-caused and fixed.** See finding 6 below.

### NOT verified — read this before assuming otherwise

- **Haptics is implemented but cannot be confirmed on this unit — hardware fault.** The firmware
  side is complete and *proven correct by register readback*; the motor does not respond because
  the driver reports its output faulted. Measured 2026-09-09 with the DRV2605's own actuator
  diagnostic (`MODE 0x06`):

  ```
  STATUS=0xA9   DIAG_RESULT=1 (actuator missing/open/short)   OC_DETECT=1 (over-current)
  ```

  Every configuration register read back exactly as written — `MODE 0x00`, `LIBRARY 0x01`,
  `WAVESEQ1 0x01`, `FEEDBACK 0x36` (ERM), `CONTROL3 0xA0` (open loop) — so the writes stick and
  the setup is right. The motor was then driven three ways: ERM waveform, LRA waveform with
  library 6, and **Real-Time Playback at full amplitude, which bypasses the effect ROM entirely**.
  Nothing was felt on any of them.

  **The ERM-vs-LRA question is answered and is not the cause** — LRA was tried and changed
  nothing. `EN` is also ruled out by inference: the DRV2605 gates its own I²C on `EN`, so a chip
  answering at `0x5A` cannot have it low. A motor is physically present in the enclosure (owner
  confirmed by eye), but that confirms one exists, not that it is wired to *this* driver or that
  its leads are intact.

  **Do not spend another round on firmware.** The remaining question is electrical and needs a
  multimeter across the motor terminals plus a look at whether those leads reach the DRV2605.
  Accepted as a nice-to-have gap rather than a blocker (owner, 2026-09-09).

- **Importing a file with no `draupnir` key.** Not exercised on device. The rejection logic is
  covered by a passing unit test (`parseEnvelope rejects a file with no draupnir key`), and the UI
  path to it — `parseEnvelope` → `TransferException` → alert dialog — is the same one the
  bad-`schema` test did exercise, so the wiring is proven and only that branch is untested. Cheap
  to close: pick any unrelated `.json`.

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

### Seven findings — do not re-derive these on the next hardware round

Each of these cost a full hardware round (flash → observe → diagnose → fix → reflash) to find. All
most are instances of something that compiles cleanly and does nothing observable — treat that symptom
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
5. **A dedup guard with no expiry is a delayed silent-drop bug.** The M5Dial's RX duplicate-write
   guard existed to absorb a doubled `onWrite()` from the BLE stack, which arrives within a
   millisecond or two — but it compared against the previous write with **no time window**, i.e.
   forever. The app's first command after reconnecting is byte-identical to its first command last
   session (`{"cmd":"get_profiles"}`), so the device bonded and reconnected flawlessly and then
   silently swallowed every command. It presented as *"cannot reconnect, and restarting the app
   doesn't help"* — the stale state is on the device, so only a reboot cleared it, which is why the
   first pair after every flash worked and hid the bug for months.

   **The port hazard is the real lesson.** `ble_engine.cpp`'s `onDisconnect` carries a comment
   saying `lastRxValue` *"needs no reset … so it self-expires."* That is true on the Waveshare,
   which has a 10 ms window, and false on the M5Dial, which had none — so the comment actively
   reassures a reader porting between the boards. When copying reasoning across the two targets,
   check that the premise holds on both. Fixed in `1c7e3b8` by porting the window.

6. **A diagnostic bus scan broke the thing it shared a bus with.** `haptics_init()` opened with an
   `i2c_scan()` probing all 112 addresses — **including `0x15`, the CST816 touch controller** —
   immediately after `Touch_Init()`. Poking the touch chip while it was still starting left it
   unable to deliver events: no `CLICKED`, no `GESTURE`, while the encoder kept working. That last
   detail is what made it misleading for so long; it looked like an LVGL or input-routing fault
   rather than a bus one, and cost the feature a milestone of being written off as blocked.

   Isolated by single-variable bisect on hardware, 2026-09-09:

   | scan | delay before `haptics_init()` | touch |
   |---|---|---|
   | yes | none | **broken** (the documented original) |
   | yes | ~1 s | works |
   | yes | none | **broken** (controlled re-test) |
   | **no** | none | **works** |

   Note the second row: a delay *also* made it work, which would have looked like a fix and
   shipped a magic number over the real cause. The delay was only letting the CST816 finish
   starting before being probed. **The scan is deleted, not delayed** — and it had already served
   its whole purpose by telling us the bus carries `0x15` and `0x5A`. Re-adding it to re-answer a
   settled question would reintroduce the bug; `haptics.cpp` says so at the deletion site.

7. **Icon bitmaps exist only for macros edited since the feature landed.** The app writes
   `icon_xbm` **only for the macro being edited** (`editor_panel.dart`), and the Waveshare renders
   icons **only** from `icon_xbm` — never from the icon *name*. In the owner's live config that is
   **2 of 24 macros with a bitmap, 20 with a name and no bitmap**, so 22 render no icon on that
   board, and the only fix today is opening and re-saving each macro by hand. Not an
   export/import bug: a transfer carries faithfully whatever exists. The cheap direction is
   app-side — generate `icon_xbm` for every macro on save, not just the edited one — but check the
   payload cost first: ~108 hex chars × 20 macros is ~2.4 KB against the Waveshare's 8 KB BLE RX
   buffer.

**Tally worth keeping in view:** seven LVGL/esp_lcd APIs have now compiled cleanly and done nothing
in this project's history, two of them (findings 1 and 2 above) in M9 alone. Finding 5 is the same
shape one layer up — not an API that silently no-ops, but a *guard* that silently over-matches.
When something "should work" and visibly doesn't, absence-not-error is the pattern to suspect first
here.

---

### Verified on hardware during M12 — Waveshare, 2026-09-24

Flashed `feat/m12-icon-rendering` to the Waveshare (COM10) at 1,177,270 bytes / 35%, and
installed the matching release APK.

- **Icons are legible at ring size.** The glyphs M11 predicted would fail at 18x18 --
  `gitPullRequest`, `alignLeft`, `listOrdered`, `braces` -- now render as 46px antialiased font
  glyphs. This was the entire point of the milestone.
- **Existing profiles were crisp with no re-save**, confirming tier 1 wins for any macro whose
  `icon` name the firmware knows. (Design criterion 5 originally said these would render at tier
  3; that was wrong and was corrected before testing -- the app has always written `icon`, so
  the font path claims them.)
- **No blank wedges.** A wedge rendering nothing is the one failure the four-tier chain cannot
  recover from, and the generator's cmap cross-check exists to prevent it.
- App and device both behaved correctly in ordinary use.

- **Design criterion 7 passed: the retired M5Dial, unflashed, still loads and renders its
  profiles.** This is the only real test of the additive-only rule -- a board that will never
  receive another firmware update, reading a document that now contains `icon_bmp48` keys it has
  never heard of, and ignoring them cleanly. ArduinoJson 7's elastic document absorbs the unknown
  key and its 16384-byte RX buffer has room to spare.

**Still open on the hardware gate:**

- **Tier 2 (the app-supplied 48x48) has never been exercised on glass.** Reaching it requires an
  `icon` name the firmware lacks, which now means deliberately adding one to the app's map. The
  stride algebra agrees in all three places and the length is pinned by tests, but only the
  panel proves the ALPHA_1BIT draw at w=48.
- **An oversized import fails correctly but silently** -- nothing on the import path renders
  `lastSaveFailure`. Surfacing it is new UI and was deliberately left out of the fix round.
- **No scoped re-review ran on the final fix round** (account rate limit). Each fix was verified
  in place by the controller instead, which is weaker than a fresh reviewer.

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

> **Stale as of 2026-09-23.** The steps below were written before M10 (export/import), M11
> (UI/UX polish, PR #16) and the M12 design (PR #17) landed, and reference PRs that have since
> merged. The current next step is **M12 — crisp icon rendering on the Waveshare**; its approved
> design is `docs/superpowers/specs/2026-09-23-waveshare-icon-rendering-design.md`, and **M13
> (OTA firmware update from the app)** sits behind it. Treat this section as history until it is
> rewritten.

**Security work is closed.** The M5Dial security gate landed on `feat/m5dial-security-gate` (PR #6)
and is verified on hardware — positive path 2026-09-06, hostile-central negative test 2026-09-07.
Both boards enforce pairing, bonding and GATT permission flags, and both claims are backed by a
real refusal test rather than by inference. Nothing security-related is outstanding. (The M5Dial
has since been retired — §2 — but shipped with its gate closed, which is why it is safe to leave
frozen rather than withdraw.)

### Step 1 — Land the rebrand

PR #12 (`feat/branding-draupnir-forge` -> `master`) is open and hardware-checked: logo, launcher
icon and brand palette, plus the release-build validation below. Merge it and `master` is current.

### Step 2 — Icon bitmaps for every macro *(small, and it fixes a visible gap)*

**22 of 24 macros in the owner's config render no icon on the Waveshare** — see §4 finding 7. The
app writes `icon_xbm` only for the macro being edited, and the Waveshare renders only from
`icon_xbm`, never from the icon name. The fix is app-side: generate the bitmap for every macro on
save. Check the payload cost against the Waveshare's 8 KB BLE RX buffer first (~2.4 KB for 20
macros).

### Step 3 — M11, publish to Google Play

Scoped but not designed.

**Publisher identity is decided (2026-09-09):**

| | |
|---|---|
| `applicationId` | **`net.holocronlabs.draupnir`** |
| Developer name | **Holocron Labs** |

Reverse-DNS of `holocronlabs.net`, a domain the owner controls, which is the convention that keeps
the namespace from colliding with anyone else's — and it leaves `net.holocronlabs.*` free for the
other apps planned under that identity. It supersedes an earlier `net.l0k1.draupnir`, changed
before first publish precisely because **it can never be changed after**: a different package is a
different app on Play, with no upgrade path for anyone who installed the first one. The same string
becomes the iOS bundle ID later (iOS is a separate, later milestone; the `ios/` platform folder
does not exist yet and needs a Mac).

**It is in the code as of 2026-09-09**, verified in the built release APK rather than the source:
the merged manifest carries `net.holocronlabs.draupnir` with no trace of `com.example`. Note that
`namespace` is not just a string — it dictates where `MainActivity` must live, so the Kotlin source
moved to `kotlin/net/holocronlabs/draupnir/` with a matching `package` declaration.

**Permission hygiene landed with it**, and was verified by parsing the merged release manifest:

| | |
|---|---|
| `INTERNET` | **removed** — release only; `src/debug` and `src/profile` keep it, which is why hot reload still works |
| `BLUETOOTH_ADVERTISE` | **removed** — this app is a BLE central; it never advertises |
| `ACCESS_FINE_LOCATION` | **bounded to `maxSdkVersion=30`** — on API 31+ `neverForLocation` replaces it, so a modern phone is never asked for location |
| `usesCleartextTraffic` | **removed** — another leftover of the cut Wi-Fi UI |

Dropping `INTERNET` is what makes "collects no data, shares no data" an honest Data Safety
declaration rather than one the manifest contradicts. `package:http` went with it, confirmed unused
first.

`.gitignore` now carries keystore patterns, deliberately **before any keystore exists** — that
ordering is the whole protection, since `.gitignore` cannot retroactively un-commit anything and
this repo is public. **No key has been generated yet**, and no signed bundle has been built. Those
are the two remaining pieces of M11a.

> **A trap worth knowing:** XML comments cannot contain `--`, which this project uses freely as an
> em-dash in C++ and Dart. The first manifest edit failed to parse for that reason alone.

The probe is already done, and it came back clean: `flutter build apk --release` and `appbundle`
both succeed, and **BLE works from the release APK** — R8 does not strip anything
`flutter_blue_plus` needs. That was the one real engineering risk. What remains is mostly
configuration and paperwork:

- `applicationId` off `com.example.*` (Play rejects it outright)
- a real keystore + Play App Signing — and **add keystore patterns to `.gitignore` BEFORE creating
  one**, or a committed `key.properties` leaks credentials into history
- permission hygiene: `ACCESS_FINE_LOCATION` is declared unbounded though `BLUETOOTH_SCAN` already
  carries `neverForLocation`; `BLUETOOTH_ADVERTISE` is boilerplate (the app is a central and never
  advertises); `INTERNET` is declared but **the app makes zero network calls** — `package:http` is
  an unused leftover from the cut Wi-Fi UI. Removing it earns a "collects no data, shares no data"
  Data Safety declaration, the easiest privacy posture available.
- privacy policy, Data Safety form, content rating, 512x512 icon, 1024x500 feature graphic,
  screenshots, and a Play Console account
- check early whether the **12 testers / 14 days closed test** requirement applies to the account;
  it is a schedule item, not a code item, and discovering it late costs two weeks

Split it: **M11a release readiness** (all in-repo, testable, ends in a signed AAB) gates **M11b
store presence** (artifacts and Console work, most of which only the owner can do).

### Step 4 — Haptics: closed as far as firmware can take it

Nothing to do here in code. The touch conflict is fixed (§4 finding 6) and `haptics_init()` is
enabled; the motor does not respond because the DRV2605 reports its output faulted, with
over-current, while every register reads back correct. ERM, LRA and full-amplitude RTP were all
tried. Recorded under NOT verified, accepted as a nice-to-have gap.

If it is ever revisited it is a **bench** task, not a coding one: meter the motor terminals and
confirm whether those leads actually run to the DRV2605. Worth re-testing on P2 hardware, where
the firmware should work unchanged if the actuator is sound.

One thing that *is* still design work, independent of the fault: `haptics_pulse()` has exactly one
call site — swipe-down kill-all, and only while a macro runs. Even with a working motor that is
not really haptic feedback. Wiring it to macro fire and selection change is the real feature.

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

- ~~**M5Dial firmware has no cryptographic gate.**~~ **CLOSED 2026-09-06.** That sketch now sets
  `ESP_LE_AUTH_REQ_SC_MITM_BOND` with `ESP_IO_CAP_OUT`, shows a passkey on screen, carries
  `_ENC`/`_AUTHEN` permission flags on RX and `_ENC` on TX, and refuses at the handler anything
  whose link `sec_state` is not `encrypted && authenticated`. The `CONFIG_MODE` check is gone —
  it was physical presence, not authentication. Wi-Fi and the LAN-reachable web API were deleted
  in the same pass, which was the larger hole of the two. Verified on hardware (§4) on both the
  positive path (2026-09-06) and by hostile-central negative test (2026-09-07) — an unbonded
  central is refused. Nothing about this is outstanding. The rebuttal below is retained because it
  remains the standing answer to anyone proposing the token.
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
- ~~**The M5Dial's `profiles.json` write is non-atomic.**~~ **CLOSED 2026-09-06.** Ported the
  Waveshare's temp→verify→rename, verifying the byte count three ways (`measureJson`,
  `serializeJson`'s return, and the re-opened file's size) before the rename, with a
  remove-then-retry fallback for builds that refuse to rename onto an existing file. Every failure
  before the rename leaves the original untouched and deliberately does not reload. The M5Dial
  keeps its **immediate** `killAllMacros()` → `loadProfiles()` rather than the Waveshare's deferred
  reload, because nothing but `loop()` touches `profilesDoc` on this board.
- **Two long-lived branches exist and they are unrelated histories.** `main` is the real trunk.
  `origin/master` is a bare "Initial commit" that holds almost none of the tree, yet it is the
  repo's *default* branch (`origin/HEAD -> origin/master`), so tooling and fresh clones land on the
  empty one. Diffing against `master` gives a meaningless "all new" result. **Use `main`** (or, for
  this milestone's own history, `feat/m9-icons-orientation-rotary` / `feat/m7-m8-persistence`).
  Worth deleting or repointing `master` before it misleads anyone else.
