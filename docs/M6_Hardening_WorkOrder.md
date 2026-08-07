# M6 — Config Hardening: Implementation Work Order

*Draupnir · L0k1.Net · issued 2026-07-24 · target: `firmware/Waveshare_LVGL_Test/` + `companion_app/`*

**Read `docs/Draupnir_Spec.md` (v3) first.** This document is the implementation contract for
milestone **M6**, which §10 of the spec marks as *blocking*. Nothing else on the roadmap should
start until this lands.

---

> ## Status — updated 2026-07-25
>
> **All seven tasks are code-complete and compile-verified. None is hardware-verified.**
> Commits, in order: `8051a7d` (H2), `c868e31` (H3), `0c09ef8` (H4), `d2edc07` (H5),
> `798fa46` (H6), `a2733c7` (H7), `3607a28` (H1). Baseline is `6373651`.
>
> **M6 is NOT complete.** The milestone's defining test — an unbonded BLE central being rejected
> — has not been run. See H1's verification section.
>
> **Two prescriptions in H1 as originally written were wrong for this core** and have been
> corrected in place below; the notes are kept rather than deleted because the reasoning matters.
> Both were caught during implementation and verified against
> `m5stack:esp32 3.3.8`'s BLE library sources.
>
> **FQBN — RESOLVED 2026-07-25.** Now recorded in `docs/Toolchain_arduino-cli.md`:
>
> ```
> esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled
> ```
>
> Requires the **Espressif** core (`arduino-cli core install esp32:esp32`), which was not
> installed — the M5Stack core alone has no suitable target, which is why the original M6 work was
> compile-checked against `m5stack_stamp_s3` as a syntax-only stand-in. The M6 firmware
> **re-compiles clean** against the real FQBN: ~970 KB (29 %) flash, ~112 KB (34 %) RAM.
>
> Hardware confirmed on-chip via `esptool flash-id`: ESP32-S3 rev v0.2, **16 MB quad flash**,
> **8 MB embedded PSRAM**. Two board quirks are documented in the toolchain doc and will waste
> your time if you meet them cold: the **USB-C plug orientation** selects which of the board's two
> MCUs you reach, and **auto-reset does not work** — download mode needs a manual BOOT press.

---

## 0. Why M6 exists

Draupnir presents itself to the host as a **USB keyboard**. The BLE config channel can currently
be driven by any central in radio range, with no pairing enforced. That is arbitrary keystroke
injection into the attached machine, plus profile exfiltration. Three of the other defects below
can permanently wedge or brick the device with no user-visible error.

M6 is the difference between a prototype and a device that is safe to leave plugged into a
machine you care about. Until it lands, **do not describe the device as usable unattended.**

---

## 1. Ground rules for whoever executes this

### Hard constraints — violating these fails the work order

1. **Do not reintroduce Wi-Fi, an HTTP server, a captive portal, or a web UI.** The web config
   path is cut permanently (spec §1, §5, §7). If a fix seems to need one, it is the wrong fix.
2. **Do not add an application-layer auth token.** The `token` field, the `pair` command, and the
   `pairingToken` NVS entry are removed by this work order, not preserved. Security is BLE
   pairing + bonding + GATT permissions. This is the whole point of task **H1**.
3. **Respect the threading rules** (spec §5, and `CLAUDE.md`). They are load-bearing and have
   already caused shipped bugs:
   - HID interfaces register **before** `USB.begin()`.
   - The macro engine is **loop()-task only**. UI callbacks run on the LVGL task and must use
     `macros_request_fire()`, never `macros_fire()`.
   - LVGL objects are touched only under `lvgl_lock()`.
   - BLE callbacks run on the BLE host task; hand off via queues/flags drained in `loop()`.
4. **Do not refactor beyond the listed scope.** No renaming, no restructuring, no "while I was in
   here". Each task below should be a reviewable diff.
5. **Do not claim a task is done without the stated verification.** Several of these can only be
   proven on hardware, and the human runs the hardware. Report what you actually observed,
   including failures. See §4.

### Baseline before starting

The Waveshare firmware may still be untracked. **Confirm a clean committed baseline exists**
(`git status`, `git log`) before modifying anything — several tasks touch the same files and you
will want a diff target.

### Build and flash

```
arduino-cli compile --fqbn <FQBN> firmware/Waveshare_LVGL_Test
arduino-cli upload -p <PORT> --fqbn <FQBN> firmware/Waveshare_LVGL_Test
arduino-cli monitor -p <PORT> -c baudrate=115200
```

Discover the FQBN with `arduino-cli board listall` — **do not guess it.** The M5Dial FQBN, for
reference, is
`m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB`.

The human handles USB plugging and reports screen/Serial behavior. The firmware already emits
`[diag]` and `[ble]` Serial traces — use them, and add more if a task needs evidence.

---

## 2. Tasks

Ordered by dependency, not severity. **H2, H3, H4 are self-contained and low-risk — land them
first.** H1 is the highest-value fix but carries known instability history, so it benefits from
the others being stable underneath it.

---

### H2 — Bound the BLE RX reassembly buffer *(start here)*

**Severity: critical. Permanently wedges the config channel.**

**File:** `firmware/Waveshare_LVGL_Test/ble_engine.cpp`, `RxCallbacks::onWrite`, ~line 209.

**Current code:**
```cpp
for (size_t i = 0; i < rxValue.length() && bleRxLen < BLE_RX_BUFFER_SIZE - 1; i++) {
```

**The defect.** When `bleRxLen` reaches `BLE_RX_BUFFER_SIZE - 1` (4095), the loop stops consuming
input — *including the `'\n'` that would terminate the message and reset `bleRxLen`*. Nothing
else ever resets it. Every subsequent BLE command is silently discarded **until reboot**. No
error is logged, no response is sent; the app just times out at 30s, forever.

**Why this is reachable, not theoretical.** The companion app writes an `icon_xbm` field into
every macro — an 18x18 1-bpp bitmap as a **108-character hex string**
(`companion_app/lib/utils/icon_generator.dart`). A macro with an icon and one action is roughly
220 bytes of JSON; sixteen of them is ~3.5 KB, before the `save_profiles` envelope. **A single
fully-populated profile lands essentially at the 4096 limit; two profiles clear it comfortably.**

**Required behavior:**
1. On overflow, **reset `bleRxLen` to 0** and discard the partial message, so the channel
   recovers instead of wedging.
2. Set a flag so the *remainder* of the oversized message (up to and including its terminating
   `'\n'`) is also discarded — otherwise the tail gets parsed as a bogus standalone command.
   Clear the flag when that `'\n'` arrives.
3. **Log it** (`Serial.printf`) with the byte count.
4. **Send an error response** to the app so it fails fast instead of waiting out its 30s timeout.
   Suggested: `{"status":"error","message":"Command too large"}`. Note this must be dispatched
   from the loop() task, not from the callback — follow the existing queue hand-off pattern.
5. **Raise `BLE_RX_BUFFER_SIZE`** to a realistic multi-profile ceiling. 8192 is the suggested
   floor; justify whatever you pick against heap headroom (see the caution below).

**Caution — heap.** Neither board has PSRAM. LVGL draw buffers, the profile `JsonDocument`, the
BLE stack, and USB all live in internal RAM. `bleRxBuf` is deliberately `malloc()`'d rather than
a static array — a static array here previously broke BLE advertising outright (see
`docs/BLE_Profile_Fetch_Debugging.md`). **Keep it heap-allocated**, check the allocation result,
and log free heap before/after. If 8192 will not allocate reliably, report that rather than
silently shrinking it.

**Verification:**
- Build a profile in the app large enough to exceed the old 4096 limit (16 macros with icons).
- Save it. It must **succeed**.
- Then artificially exceed the *new* limit (temporarily lower the constant, or add profiles).
  The device must log the overflow, return the error, and — critically — **still respond
  correctly to the very next `get_profiles`**. That last step is the actual regression test; the
  old bug's signature is that everything after the overflow goes silent.

---

### H3 — Stop running macros before a profile reload

**Severity: critical. Use-after-free.**

**Files:** `firmware/Waveshare_LVGL_Test/macro_engine.cpp` (`profiles_reload`, `macros_fire`,
`macros_update`), and `ble_engine.cpp` (`save_profiles` handler, ~line 172).

**The defect.** `macros_fire()` stores `JsonObject macroDef` (~line 291) — a *reference into*
`profilesDoc`. `profiles_reload()` re-runs `deserializeJson(profilesDoc, file)`, which clears and
reallocates the document's memory pool. **Nothing clears `runningMacros[]`.** The next
`macros_update()` tick dereferences the stale object at ~line 312 and reads freed memory.

**Why it is deterministic, not a rare race.** A macro with `mode: "toggle"` **never terminates** —
`macros_update()` resets `currentActionIndex` to 0 and loops forever until the macro is fired
again (~line 314). The shipped default profile contains exactly such a macro (`pos: 4`,
"Caps Lock"). So: fire the toggle macro, then save profiles from the app → guaranteed
use-after-free.

**Required behavior:**
1. Add a `macros_stop_all()` to the macro engine that clears every `runningMacros[]` entry
   (`active = false`, `macroDef` reset to a default-constructed `JsonObject`, indices zeroed).
2. Call it at the **top of `profiles_reload()`**, before the deserialize — not from the caller, so
   no future call site can forget.
3. **Release any held HID state before clearing.** A macro interrupted mid-sequence may have
   modifiers or mouse buttons down. Call `Keyboard.releaseAll()` and release mouse buttons, or the
   host is left with a stuck Ctrl. This is a real symptom, not a hypothetical.
4. Declare it in `macro_engine.h` with a comment stating the loop()-task-only constraint.

**Verification:** fire the "Caps Lock" toggle macro so it is actively looping, then save profiles
from the app. Device must not crash or reset, the ring must rebuild, and the host must not be
left with a stuck modifier. Watch Serial for resets during the save.

---

### H4 — Atomic, recoverable profile writes

**Severity: high. Corruption is currently unrecoverable without a reflash.**

**Files:** `ble_engine.cpp` (`save_profiles` handler, ~lines 165-175), `macro_engine.cpp`
(`profiles_reload`, ~lines 71-96).

**Two distinct defects:**

**(a) The write is not atomic and is unchecked.** `save_profiles` opens `/profiles.json` with `"w"`
— truncating the only good copy — then `serializeJson(profilesObj, f)` with **no check of the
returned byte count** and no check that `f.close()` flushed. A power loss, a full filesystem, or a
short write leaves a truncated, unparseable file.

**(b) Parse failure is unrecoverable.** `profiles_reload()` regenerates the built-in default only
when the file fails to **open** (~line 73). On a **parse error** it merely logs and returns
(~lines 86-89), leaving `profilesDoc` in whatever state a failed deserialize left it. There is no
path back to a working config short of reflashing.

**Required behavior:**

For (a) — write-temp-then-rename:
1. Serialize to `/profiles.json.tmp`.
2. **Check the byte count returned by `serializeJson` is non-zero and matches expectations**, and
   that the file closed cleanly.
3. Only then `LittleFS.remove("/profiles.json")` and `LittleFS.rename(...)` into place.
4. On any failure: remove the tmp file, leave the original untouched, and return an error to the
   app. **Do not call `profiles_reload()` on the failure path.**

For (b) — fallback on parse failure:
1. If `deserializeJson` fails, log it loudly, then **fall back to the built-in
   `defaultProfilesJson`**: rewrite the file with it and load that.
2. Factor the "write defaults and load them" logic so both the missing-file and parse-failure
   paths use it.
3. Also handle the currently-unguarded case where the `"w"` open in the missing-file path itself
   fails — right now the code calls `file.print()` on an invalid `File` and proceeds.

**Verification:**
- Save a valid profile set; confirm it round-trips through `get_profiles`.
- Manually corrupt `/profiles.json` (truncate it, or write garbage via a temporary test hook),
  reboot, and confirm the device falls back to defaults and **boots to a working ring** rather
  than an empty or crashed UI.
- Confirm `/profiles.json.tmp` is not left behind after a successful save.

---

### H5 — Fix the pairing overlay dropping its state change

**Severity: high. Blocks H1 from being usable.**

**File:** `Waveshare_LVGL_Test.ino`, `update_pairing_overlay()`, ~lines 229-235.

**Current code:**
```cpp
if (isPairing == wasPairing) return;
wasPairing = isPairing;          // committed BEFORE the lock is acquired
if (!lvgl_lock(100)) return;     // lock fails -> the transition is lost forever
```

**The defect.** `wasPairing` is updated before `lvgl_lock(100)` is attempted. If that 100 ms lock
acquisition times out even once, the state transition is consumed and never retried — the overlay
never appears, so **the passkey is never displayed and the user cannot pair.**

**Required behavior:** acquire the lock first; only commit `wasPairing` after the LVGL update has
actually been applied. On lock failure, leave `wasPairing` unchanged so the next `loop()` tick
retries.

**Note:** this is directly on H1's critical path — passkey pairing is unusable if the PIN never
renders. Fix it before attempting H1's hardware verification.

---

### H1 — Enforce BLE pairing on the config characteristics *(highest value)*

**Severity: critical. Unauthenticated keystroke injection into the host.**

**Files:** `ble_engine.cpp` (`ble_init`, ~lines 296-315; `ServerCallbacks::onConnect`, ~line 229),
plus companion app changes below.

**The defect.** `handleBleCommand()` never checks any authorization, and the config
characteristics carry **no encryption/authentication permission flags** — see the comment at
~line 301 documenting that `PROPERTY_READ_ENC` / `PROPERTY_WRITE_ENC` were deliberately dropped
as a diagnostic step. Security is only *requested*, via `BLESecurity::startSecurity()` in
`onConnect`. **A central that ignores that request can still write to the RX characteristic and
issue `save_profiles` / `trigger` commands.**

Meanwhile the app sends a `token` field on every request that the firmware entirely ignores, and
sends a `cmd: "pair"` that this firmware does not implement — so the app's pairing flow is not
merely insecure, it is **dead code that always reports failure**.

**Required behavior:**

**Firmware:**
1. Require an encrypted *and* authenticated (MITM-protected) link on **both** the RX and TX
   characteristics. Flags on the characteristic itself are what the stack actually enforces; a
   security *request* at connect time is not.

   > **Corrected 2026-07-25.** This originally said "set GATT access permissions" and pointed at
   > `setAccessPermissions()`. **That method does nothing on this core** — its entire body is
   > wrapped in `#ifdef CONFIG_BLUEDROID_ENABLED` (`BLECharacteristic.cpp:167-171`), and
   > `m_permissions` only exists under Bluedroid. NimBLE builds the attribute flags from the
   > **properties bitmask** instead (`BLEService.cpp:632` assigns `flags = m_properties`).
   >
   > The enforcement bits therefore go in the properties argument to `createCharacteristic()`:
   > `PROPERTY_WRITE_ENC` / `PROPERTY_WRITE_AUTHEN` (and the `READ_` equivalents), which resolve
   > to real `BLE_GATT_CHR_F_*` values under NimBLE and to `0` under Bluedroid. Following the
   > original instruction would have compiled, looked correct, and enforced nothing.

2. Protect the CCCD so an unbonded central cannot subscribe to notifications.

   > **Corrected 2026-07-25.** This originally said to protect the `BLE2902` descriptor.
   > **On this core there is no such descriptor to protect:** `addDescriptor()` early-returns for
   > UUID 0x2902 under NimBLE (`BLECharacteristic.cpp:126-134`), which auto-creates the CCCD
   > itself. The pre-existing `addDescriptor(new BLE2902())` call was dead code and was removed.
   >
   > Gate the auto-created CCCD with `BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC` (0x8000) on the TX
   > characteristic instead.
   >
   > **Known, accepted gap:** `BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN` is `0x10000`, and
   > `BLECharacteristic` stores properties in a `uint16_t` (`esp_gatt_char_prop_t`), so that bit
   > truncates to zero and cannot be applied through this wrapper. The CCCD is therefore gated on
   > encryption but not explicitly on authentication. Not exploitable **as currently configured**
   > — `ESP_LE_AUTH_REQ_SC_MITM_BOND` means this device will not complete a Just Works pairing, so
   > any encrypted link is necessarily authenticated — but **this must be revisited if the auth
   > mode is ever relaxed.**
3. Keep `ESP_LE_AUTH_REQ_SC_MITM_BOND` and `ESP_IO_CAP_OUT` (display-only). The passkey is shown
   on screen; the user types it into the phone.
4. **Remove** all remnants of the application-layer scheme: any `token` handling, any `pair`
   command, and the `pairingToken` NVS key. Also remove them from `firmware/M5_M6_config/` so the
   two boards do not diverge on the security model.
5. Reconsider `regenPassKeyOnConnect(false)` (~line 293). It is currently disabled as part of the
   same diagnostic rollback. A fixed-for-uptime passkey is weaker than a per-pairing one; if
   bonding works correctly the passkey is only entered once anyway. **Test with it re-enabled**,
   and if it reintroduces instability, leave it off and *document why* in a code comment rather
   than silently.

> **API caution — the reason two prescriptions above were wrong.** This build uses the
> Bluedroid-styled wrapper classes (`BLEDevice`, `BLECharacteristic`, `BLE2902`) but is
> **NimBLE-backed** on the installed core (`m5stack:esp32 3.3.8`) — confirmed in the build's
> `sdkconfig.h` and noted at `ble_engine.cpp:261`.
>
> The dangerous consequence: the wrapper **keeps the Bluedroid-shaped API surface and silently
> neuters it.** `setAccessPermissions()` compiles and does nothing. `PROPERTY_READ_ENC` and
> friends are `#define`d to **`0`** on the Bluedroid branch of `BLECharacteristic.h` and to real
> `BLE_GATT_CHR_F_*` values on the NimBLE branch. `addDescriptor(new BLE2902())` compiles and is
> discarded. Every one of these fails **silently and in the insecure direction** — nothing warns,
> nothing errors, and the device advertises and connects exactly as before.
>
> Most online examples are Bluedroid and are actively wrong here. **Read the installed core's
> headers and sources before using any BLE security API** — `BLECharacteristic.h`,
> `BLECharacteristic.cpp`, `BLEService.cpp`. Do not guess, and do not trust that a call which
> compiles has taken effect. Where a security flag cannot be verified in the sources, say so
> rather than assuming.

> **Known-history caution — read before flashing.** Per the comments at `ble_engine.cpp:288-293`
> and `301-304`, the ENC permission flags plus `regenPassKeyOnConnect` were removed *because they
> correlated with instability*: repeated unexplained resets into the ROM bootloader, and one
> `get_profiles` exchange the firmware logged as fully sent+acked that the app never received.
> That root cause was **never identified** — it was rolled back, not fixed. Expect it to
> resurface. Re-enable the flags **incrementally**, one change per flash, with Serial captured
> throughout (`scripts/serial_capture.ps1`). If the resets return, capture the boot reason and
> report it — do not simply revert to unauthenticated and call the task done. Some of the earlier
> instability may in fact have been H2/H3/H4, which is part of why those land first.

**Companion app** (`companion_app/lib/state/draupnir_state.dart`):
6. Delete the `pair()` method and the `cmd: 'pair'` request (~line 431-468).
7. Remove the `token` field from `get_profiles`, `save_profiles`, and `trigger` (~lines 398, 480,
   509), and remove `authToken` / its `SharedPreferences` persistence.
8. Rework `needsPairing` / the `'Unauthorized'` handling. Pairing is now the OS's job — the app
   should surface a clear "pair this device in your phone's Bluetooth settings, then enter the PIN
   shown on the knob" message when the link is not encrypted, rather than offering an in-app
   Pair button. Update `dashboard_screen.dart` accordingly.
9. **Remove the HTTP/Wi-Fi code paths** (`http.get`/`http.post` to `/api/profiles`, `/api/pair`,
   `/api/trigger`, `deviceIp`, `_headers`, `isBluetooth` branching). They are the web UI's client
   half and the web UI is cut. This simplifies every method to a single transport.

**Verification (requires hardware + a phone):**
- Fresh device, unbonded phone: connecting must display the passkey overlay on the knob, and the
  phone must prompt for it. Entering it correctly must complete bonding.
- After bonding, `get_profiles` / `save_profiles` / `trigger` all work.
- Power-cycle both: reconnect must be silent (bond retained), no passkey re-entry.
- **Negative test — this is the one that proves the fix.** Using a generic BLE tool (nRF Connect
  or similar) from an *unbonded* device, connect and attempt to write to the RX characteristic and
  to subscribe to TX notifications. **Both must be rejected with an insufficient-authentication
  error.** If either succeeds, H1 is not done regardless of what the paired app can do.

---

### H6 — RX duplicate-write guard can silently drop valid data

**Severity: medium. Silent data corruption.**

**File:** `ble_engine.cpp`, `RxCallbacks::onWrite`, ~line 205.

```cpp
if (rxValue == lastRxValue) return; // duplicate callback for the same write
```

**The defect.** This assumes two identical consecutive writes are always a stack artifact. The app
sends `save_profiles` in **180-byte chunks** (`draupnir_state.dart:192`). With repetitive JSON —
runs of similar macro objects, long `icon_xbm` hex — two genuinely identical adjacent chunks are
possible, and the second is silently discarded, corrupting the command.

**Required behavior:** replace value-equality de-duplication with something that distinguishes a
retransmit from legitimately identical data. Preferred: a **sequence number on inbound chunks**,
mirroring the `[0xFE, seq]` scheme already used outbound — the app increments per chunk, the
firmware ignores a repeat of the last-seen seq. This requires a matching app change.

If a full inbound sequence scheme is too large for M6, an acceptable interim fix is to scope the
guard far more tightly (e.g. only suppress a repeat arriving within a few milliseconds) and
**document the residual risk in a comment**. State clearly in your report which option you chose.

---

### H7 — `get_profiles` failure sends no response *(lowest priority)*

**File:** `ble_engine.cpp`, `get_profiles` handler, ~line 153.

When `sink.flushRemainder()` fails after ack retries are exhausted, the device logs it but sends
nothing. The app waits out its full 30-second timeout with no diagnostic.

**Required behavior:** on sink failure, attempt a short error response
(`{"status":"error","message":"Send failed"}`). Note this may itself fail if the link is genuinely
broken — that is acceptable; best-effort is the goal.

---

## 3. Explicitly out of scope for M6

Do not start these; they are M7+ in spec §10. Listed so they are not "helpfully" folded in:

- **NVS persistence** of `activeProfile` and brightness (M7). Note `macro_engine.cpp:92` *reads*
  `activeProfile` and nothing ever writes it — real, but not M6.
- **On-device profile switching** with directional indicators (M8).
- **Icon rendering** on ring wedges, and encoder detent alignment (M9).
- Applying `settings.brightness` instead of the hardcoded `LCD_PWM_MODE_255`
  (`Waveshare_LVGL_Test.ino:313`) (M10).
- Changing the default profile's "Caps Lock" macro away from `toggle` mode — a real papercut,
  but a content change, not hardening.
- Deleting `firmware/Waveshare_Knob_Config/` and its `refactor*.py` scripts.
- The `loop()`-blocking issues (BLE ack retries up to 5x800ms; `delay(8)` per character in text
  macros). Real, but a design change deserving its own milestone.

---

## 3a. Follow-ups created by M6 *(added 2026-07-25)*

Recorded so they are not lost. None blocks M6 sign-off; the first is the most important.

- **The M5Dial firmware's BLE config channel now has no cryptographic gate.** H1 removed the
  `pairingToken` scheme from `firmware/M5_M6_config/` for consistency, but that sketch has **no
  `BLESecurity` setup and no GATT permission flags at all** — verified: the only `BLESecurity`
  string in the file is an explanatory comment. Its config access is now gated solely on
  `CONFIG_MODE`. This is close to the prior behaviour (the token check was bypassed whenever
  `currentMode == CONFIG_MODE`) but it means the *second* supported board still carries exactly
  the vulnerability M6 exists to close. **Port the Waveshare security block to it.** Note also a
  real behaviour change: a valid token previously allowed config access *outside* CONFIG_MODE;
  that path is gone.
- **`_looksLikeAuthFailure` in the companion app is an untested string heuristic.** An
  insufficient-authentication rejection surfaces as a platform GATT error rather than a device
  response, so the app matches it loosely. Verify against the error text Android/iOS actually
  return and tighten it.
- **`http` and `shared_preferences` are now unused in `companion_app/pubspec.yaml`.** Left in
  place deliberately rather than as a drive-by dependency change; safe to remove.
- **H6 shipped the interim fix, not the sequenced one.** The RX duplicate-write guard is scoped by
  a 10 ms window rather than by inbound sequence numbers, because the same app also talks to
  `firmware/M5_M6_config/`, whose reassembler would not understand seq-prefixed chunks. Residual
  risk is documented in the code. Revisit if both firmwares ever move together. Note also that
  the NimBLE write path appears to call `onWrite` exactly once per ATT write with no
  prepare/execute double-dispatch, which would make the guard vestigial here — unconfirmed on
  hardware, which is why it was kept.
- **Record the Waveshare FQBN** in `docs/Toolchain_arduino-cli.md`. See the status block at the
  top of this document.

---

## 4. Reporting requirements

For each task H1-H7, report:

1. **What changed** — files and a summary of the diff.
2. **How it was verified** — the specific test performed and **what was actually observed**,
   including Serial output where relevant. "Should work" is not verification.
3. **What was not verified and why** — e.g. "negative BLE test not run, no second BLE device
   available." An honest gap is fine; an unstated gap is not.
4. **Anything discovered that contradicts this work order.** These defects were identified by code
   reading, not by running the hardware. If a line reference is stale or a defect does not
   reproduce, **say so and stop** rather than implementing a fix for a problem that is not there.

**Do not report M6 complete** until H1's negative test (unbonded central rejected) has been run,
or its absence has been explicitly flagged. That test is the milestone's reason for existing.
