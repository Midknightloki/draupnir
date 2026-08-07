# Draupnir — Session Handoff

*Written 2026-07-25. Pick up here.*

This is a continuation brief for whoever works on Draupnir next — another agent, a fresh session,
or the owner. It assumes **no prior context**.

---

## 1. Read these first

| Document | What it gives you |
|---|---|
| `CLAUDE.md` / `AGENTS.md` | Persona, locked decisions, threading rules, FQBNs. Identical copies — **edit both together.** |
| `docs/Draupnir_Spec.md` (v3) | The brief. Concept, hardware, data model, BLE protocol, milestones. |
| `docs/M6_Hardening_WorkOrder.md` | The current milestone's implementation contract, with per-task verification steps. |
| `docs/Toolchain_arduino-cli.md` | **Read before touching hardware.** Board quirks below will otherwise cost you hours. |

---

## 2. The goal

Draupnir is a **USB-HID macro controller in a knob** — round touch screen plus rotary encoder,
driverless over USB HID, configured from a Flutter phone app over BLE. Macros show as a ring of
colored wedges; rotate to select, tap center or tap a wedge to fire.

**The immediate goal is finishing M6 — config hardening.** The device presents to the host as a
*keyboard*, and its BLE config channel currently accepts commands from any central in radio range.
That is arbitrary keystroke injection into the attached machine. M6 closes that, plus four other
defects that can silently wedge or brick the device.

**M6 is not complete.** Code is written and flashed; the test that defines the milestone has not
been run. See §5.

---

## 3. Where things stand

Branch `main`, 13 commits ahead of `origin/main` (**nothing has been pushed**).

```
fe67318  docs: Waveshare FQBN + board quirks          <- HEAD
bfe0d4c  docs: correct two wrong BLE prescriptions
3607a28  fix(m6): H1 — enforce BLE pairing            <- NOT yet flashed
7ff11b7  chore: pre-existing companion-app / M5 WIP
a2733c7  fix(m6): H7                                   <- FLASHED, RUNNING, STABLE
798fa46  fix(m6): H6
d2edc07  fix(m6): H5
0c09ef8  fix(m6): H4
c868e31  fix(m6): H3
8051a7d  fix(m6): H2
6373651  feat: Waveshare firmware baseline            <- bisect floor
7f6f7cc  docs: M6 work order
2da815e  docs: spec v3
```

### H3 — VERIFIED ON HARDWARE 2026-07-26 ✅

The use-after-free fix is **confirmed**, twice, in the exact scenario that was previously a
guaranteed crash: a `toggle` macro actively looping when `save_profiles` triggers a reload.

```
22:17:48  [fire] fire pos=3 START mode=toggle
22:17:50  loop alive ... any_running=1          <- macro confirmed running
22:17:52  [ble] cmd: save_profiles
22:17:52  [ble] save_profiles: committed 903 bytes
22:17:52  profiles_reload: deserializeJson done, err=Ok     <- the pool realloc
22:17:53  rebuild_ring_layout: active_count=4
22:17:53  loop alive ... any_running=0          <- macros_stop_all() dropped the stale ref
```

Repeated at 22:18:20 with the macro having looped 22 s first. **No crash, no reset, no panic —
no `rst:0x` line in 90 s of capture.** The `any_running` 1→0 transition immediately after each
reload is the direct evidence: the running macro was deliberately stopped rather than left
holding a `JsonObject` into a freed pool.

No leak across cycles: resting heap 61304 → 61136 → 61128, stepping with JSON document size
(903 → 919 bytes), not per-cycle.

Also confirmed in the same session: **H4's atomic write** (`committed N bytes` — serialize to
tmp, verify, rename) fired on every save.

### Verified on hardware ✅

`a2733c7` (H2–H7, **no security changes**) was flashed and confirmed working:

- Boots to the ring UI correctly (owner-observed).
- Serial shows `[diag] loop alive` every ~3 s.
- **Heap stable at 62,752 bytes across 5 consecutive samples** — no leak, no drift.
- No resets, no crash output.

**This is a meaningful result.** The known history of unexplained resets into the ROM bootloader
was the main risk hanging over M6. H2 (buffer bounds), H3 (use-after-free), and H4 (atomic writes)
are all in this build and it is stable — consistent with the theory that some of that instability
*was* those defects. Not proof, but the risk is materially lower than it was.

### H1 + H3 (post-`bug_003`) — VERIFIED ON HARDWARE 2026-08-07 ✅

Flashed `0b02a3f`. Captured with a serial channel that no longer resets the board (see §7).

**H1 positive path.** Pairing completes with authentication and bonding:

```
[ble] show passkey: 885833
[ble] authentication complete, encrypted=1 authenticated=1 bonded=1
[ble] cmd: {"cmd":"get_profiles"}   -> streamed 947 bytes
```

`authenticated=1` is the important field: MITM, **not** Just Works. That empirically confirms the
`SC_MITM_BOND` reasoning in `ble_engine.cpp` that was previously only a reading of the config —
so the `NOTIFY_INDICATE_AUTHEN` truncation is not exploitable *in this configuration*. Bonded
reconnect then takes **105 ms** with no passkey prompt (vs ~29 s to pair fresh).

**H3 with the relocated reload.** Save arriving while a toggle macro is running:

```
any_running=1  (~24 s)
[ble] cmd: {"cmd":"save_profiles",...}
[ble] save_profiles: committed 919 bytes                      <- H4 atomic write
[ble] save_profiles: written; reload deferred to the display task  <- bug_003: engine only flags
[diag] profiles changed via BLE, reloading + rebuilding ring UI    <- .ino, under lvgl_lock
[diag] profiles_reload: deserializeJson done, err=Ok
[diag] rebuild_ring_layout: active_count=4
any_running=0                                                  <- stopped across the reload
```

No crash, no reset; a later trigger ran normally. H3, H4 and the `bug_003` relocation all hold
under the exact race they exist for.

### NOT verified ❌

- **The negative test — the milestone's definition of done — has not been run.** An unbonded
  central (nRF Connect declaring NoInputNoOutput) must be *rejected* on an RX write AND on a TX
  subscribe. Failed pairings observed so far disconnected on their own rather than being refused
  by the GATT layer, which is suggestive but not conclusive. **M6 is not complete until this
  runs.**
- **Heap across a save/reload cycle is not yet proven flat.** One cycle went 61,720 -> 61,392
  (−328 B), consistent with a larger `profilesDoc` (919 B) rather than a leak — the same "steps
  with document size, not per cycle" pattern seen before. One cycle cannot separate the two;
  soak it over many.
- H2's overflow-recovery test and H4's corruption-recovery test still have not been run.

---

## 4. Hardware — read before plugging anything in

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
**Do not use `arduino-cli monitor`** — it treats non-interactive stdin as an immediate quit.

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

## 5. What to do next, in order

### Step 1 — Run H2/H3/H4's functional tests against the flashed build

The board is already running `a2733c7`. Before adding H1's variables, confirm the fixes actually
*work*, not just that they boot. Each test is specified in the work order:

- **H3** is the cheapest and most decisive: fire the "Caps Lock" toggle macro so it loops, then
  save profiles from the app. Pre-H3 this was a guaranteed use-after-free.
- **H2**: save a 16-macro-with-icons profile (must succeed); then force an overflow and confirm the
  device still answers the *next* `get_profiles` — that is the real regression test.
- **H4**: corrupt `/profiles.json`, reboot, confirm fallback to defaults and a working ring.

### Step 2 — Flash H1 (`3607a28`) and watch for resets

```
git checkout 3607a28
arduino-cli compile --fqbn "<FQBN>" firmware/Waveshare_LVGL_Test
arduino-cli upload -p <PORT> --fqbn "<FQBN>" firmware/Waveshare_LVGL_Test
# then PHYSICALLY REPLUG, then capture serial
git checkout main
```

If resets return, **walk the rollback ladder written into the `ble_init()` comment** — one step per
flash — and report the boot reason. Do not stop at "reverted to unauthenticated and it works".

### Step 3 — The negative test *(this is what "M6 done" means)*

From an **unbonded** device using nRF Connect or equivalent: attempt to **write the RX
characteristic** and to **subscribe to TX notifications**. **Both must be rejected** with
insufficient authentication/encryption.

If either succeeds, H1 is not done regardless of how well the paired app behaves. Until this test
passes, **do not describe the device as safe to leave plugged in.**

### Step 4 — Then, and only then, M7+

NVS persistence (M7), on-device profile switching (M8), icons on the ring + encoder detent
alignment (M9). Spec §10.

---

## 6. Hard constraints — do not violate

1. **No Wi-Fi, no HTTP server, no captive portal, no web UI.** Permanently cut. If a fix seems to
   need one, it is the wrong fix.
2. **No application-layer auth token.** H1 *removed* the `token`/`pair`/`pairingToken` scheme.
   Security is BLE pairing + bonding + GATT permission flags. Do not reintroduce it as a fallback.
3. **Threading rules** (spec §5): macro engine is loop()-task only; UI callbacks use
   `macros_request_fire()`; LVGL only under `lvgl_lock()`; BLE callbacks hand off via queues; stop
   running macros before a profile reload.
4. **Verify BLE APIs against the installed core's headers.** This build uses Bluedroid-styled class
   names but is **NimBLE-backed**, and the wrapper keeps the Bluedroid API surface while silently
   neutering it — `setAccessPermissions()` compiles and does nothing, `PROPERTY_*_ENC` is `0` on the
   Bluedroid branch, `addDescriptor(BLE2902)` is discarded. All fail *silently and insecurely*.
   This already caused two wrong prescriptions in the work order.
5. **Do not claim hardware verification you did not perform.** State plainly what was observed and
   what was not.

---

## 7. Open items and known gaps

- **M5Dial firmware has no cryptographic gate.** That sketch has no `BLESecurity` setup and no
  GATT permission flags at all — config access is gated solely on `CONFIG_MODE`. The second
  supported board still carries the vulnerability M6 exists to close. **Top follow-up.**
  (Work order §3a.) Deferred by the board sequencing (spec §3), not by tooling — the hardware is
  on hand.
  > **Do not close this by restoring the `pairingToken`.** An external audit (2026-08) called the
  > removal a blocking regression; it was not. The token was checked *only outside* `CONFIG_MODE`,
  > and `CONFIG_MODE` is the only mode serving config commands — so in the mode that mattered
  > there was never a check. Accepted requests went from `{CONFIG_MODE: any} ∪ {RUN_MODE: token}`
  > to `{CONFIG_MODE: any}`: a strict subset, i.e. net *tightening*. The old `pair` command also
  > handed the token to any central in `CONFIG_MODE` and persisted it to NVS, converting one
  > moment of physical access into permanent remote `RUN_MODE` access. Full reasoning is in the
  > header comment of `firmware/M5_M6_config/M5_M6_config.ino`.
  > *Why the audit misread it:* this file's baseline commit is empty, so all 1,431 lines appear
  > as additions and the token's absence reads as a deletion with no visible context.
- **TX CCCD is gated on encryption, not authentication.** `BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN`
  (0x10000) truncates away — `BLECharacteristic` stores properties in a `uint16_t`. The argument
  that this is harmless (`SC_MITM_BOND` won't complete Just Works, so any encrypted link is
  authenticated) is **a reading of the config, not an observation** — the H1 negative test has
  never been run. Don't lean on it until §5 step 3 is done. Re-flagged by the 2026-08 audit.
- **~~Unexplained resets into the ROM bootloader~~ — SOLVED 2026-08-07, verified on hardware.**
  It was `scripts/serial_capture.ps1`, via the core's four-state DTR/RTS bootloader-restart
  machine: a .NET `SerialPort` open walks three steps and `Close()` supplies the fourth, so the
  board reset **on close** — the guilty capture looked clean and the *next* one died with
  `The port is closed`. Fixed by `Serial.enableReboot(false)`. See
  `docs/Toolchain_arduino-cli.md`. Two prior diagnoses ("port contention, a harness quirk", and
  a state-machine reading that cleared the script) were both wrong in the same direction —
  treating it as noise. What settled it was checking the *board's enumeration* after a failure
  instead of reasoning about the code. **Do that first next time.**
  > This is the leading explanation for H1's GATT flags having been rolled back once as
  > suspected of causing resets. Re-examine that assumption rather than inheriting it.
- **PSRAM is disabled although 8 MB is present.** Deliberate — kept out of the M6 stability
  diagnosis. Now that H2–H7 is confirmed stable, enabling `PSRAM=opi` is a reasonable isolated next
  experiment; free heap is 62 KB, which is workable but not generous.
- **500 MB microSD installed, unused.** Recommendation: keep `profiles.json` in LittleFS (internal,
  always present, no eject/corruption risk, and H4 just hardened that path). The SD is a good home
  for **icon assets** at M9, which is the one thing likely to outgrow internal flash.
- **The app finds the device by scanning ONLY** (`draupnir_state.dart:248-287`) and never consults
  `FlutterBluePlus.systemDevices`. A peripheral does not advertise while a link is open, so if
  Android holds a stale ACL connection the scan returns nothing and the app reports "No Draupnir
  found" — presenting as *"it paired but the app won't connect."* **Observed 2026-08-07:** the
  app connected on the first attempt immediately after a lingering link dropped (`[ble]
  disconnected` at 00:46:53). Fix: check `systemDevices` first, fall back to scanning. Same root
  cause as the earlier "no way to reconnect without restarting the app" complaint.
- **`_looksLikeAuthFailure`** in the companion app is an untested string heuristic against platform
  GATT error text.
- **`http` / `shared_preferences`** are now unused in `companion_app/pubspec.yaml`.
- **H6 shipped the interim fix** (10 ms time-scoped duplicate guard) rather than inbound sequence
  numbers, because the same app also talks to the M5Dial firmware. Documented in code. The 2026-08
  audit re-flagged the silent-drop risk and urged prioritising seq numbers — but **check first
  whether the guard is needed at all.** NimBLE calls `onWrite()` once per write, with none of the
  Bluedroid prepare/execute double-dispatch the guard was written for. If that holds on hardware,
  deleting the guard kills the silent-drop risk for free and saves a two-firmware app change.
- **Haptics remain disabled** (breaks the CST816). A three-step single-variable re-enable plan is
  written at the call site in `Waveshare_LVGL_Test.ino`. Test by tapping *and* swiping — the
  encoder kept working right through the original failure, so it proves nothing.
- **Display controller discrepancy:** third-party sources describe the panel as ST77916; the
  firmware drives it with SH8601 and works. Unresolved, low priority.
- **`firmware/Waveshare_Knob_Config/`** is a superseded Adafruit_GFX port, still untracked, with
  leftover `refactor*.py` scripts. Safe to delete once nothing is owed to it.
- **Branch `review/waveshare-m6-foundation` is pushed** and open as PR #1 against `master`. Note
  `origin/master` is a bare "Initial commit" — it holds almost none of the tree, which is why
  diff-scoped reviews of this PR see nearly every file as wholly new and lose all before/after
  context. Weigh review findings against the file's own history, not the PR diff.
