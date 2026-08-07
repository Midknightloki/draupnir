# BLE `get_profiles` / `save_profiles` Reliability — Debugging Session Handoff

**Status as of this writing:** BLE chunked-transfer reliability, the `get_profiles` heap
corruption, and a `save_profiles` RX corruption bug (found and fixed 2026-07-17) are all
**fixed and verified on hardware**. See the 2026-07-17 updates near the bottom for both.

Uncommitted changes exist in:
- `firmware/M5_M6_config/M5_M6_config.ino`
- `companion_app/lib/state/draupnir_state.dart`
- `companion_app/lib/screens/dashboard_screen.dart` (toolbar/profile-switcher redesign, largely
  from the 2026-07-17 session — see that update)

## Original symptom

Companion app → Draupnir BLE `get_profiles` fetch reliably failed. The app's debug log showed
either a 30s timeout with an empty buffer, or a buffer with only a fragment of the expected JSON.

## Root causes found, in the order they were discovered

### 1. `notify()` has no delivery guarantee (fixed)
The firmware's chunked BLE response used `BLECharacteristic::notify()` in a tight loop with a
fixed `delay(50)` between chunks. Notify is fire-and-forget at the BLE spec level — nothing
guarantees the central actually receives it, and this project's real-world environment (dense
2.4GHz surroundings, WiFi+BLE running concurrently) dropped chunks in practice. Confirmed via
firmware serial logs: `notify()` reported `SUCCESS_NOTIFY` (meaning only "handed to the
controller") even when the phone's own Android BLE log showed zero `onCharacteristicChanged`
events for that data.

**Do not re-attempt `BLECharacteristic::indicate()` as the fix.** It was tried and reverted —
this library version's `indicate()` has a real deadlock bug: its internal confirm-gate semaphore
takes with `portMAX_DELAY`, and if a single confirmation ever times out, that gate is never
released, permanently wedging all future sends until the device reboots. Verified by reading
`BLECharacteristic.cpp` in the installed `m5stack:esp32` core (v3.3.8) and by reproducing a full
device hang/reboot when it was tried.

**Fix implemented:** application-level chunk acknowledgement over notify(), with a sequence
number to disambiguate retries from duplicates:
- Firmware prefixes every TX chunk with a 1-byte sequence number, then calls `notify()`, then
  blocks (via a length-1 FreeRTOS queue, `bleAckQueue`) waiting for the app to write back
  `[0xFE, seq]` on the RX characteristic before advancing to the next chunk.
- If the ack doesn't arrive within ~800ms, the firmware resends the *same* chunk (up to 5
  attempts) rather than giving up immediately — since a missing ack usually means the original
  packet was silently dropped, not that anything is wrong with the app.
- The Dart client (`_onBleDataReceived` in `draupnir_state.dart`) tracks `_lastProcessedSeq` and
  skips re-appending a chunk whose sequence it already processed (handles the resend-after-actually-
  arrived case), but still acks it so the firmware unblocks.
- See `sendNotifyAndWaitAck()` / `sendBleMessage()` in the `.ino` file, and `_onBleDataReceived()`
  in the Dart file.

### 2. Stale-ack race in the retry loop (fixed)
Early version of the ack-wait loop treated *any* non-matching ack as "no ack, resend now,"
which sometimes came from a late duplicate ack for the *previous* chunk (the app naturally sends
an ack for every chunk it receives, including duplicates from a firmware retry). This caused
spurious resends. **Fix:** on a mismatched ack, keep waiting out the *remaining* time budget of
the current attempt instead of immediately resending — a stale ack now just gets logged and
discarded, not treated as a timeout. See the `while (true) { ... }` inner loop inside
`sendNotifyAndWaitAck()`.

Residual "stale ack" log lines are expected and harmless — verified across a full test session
that every transfer still completes (`BLE TX: sent`) despite frequent stale-ack log noise. The
double `SUCCESS_NOTIFY` printed per single `notify()` call (visible in serial logs) is still
unexplained but appears benign — not investigated further since it doesn't affect correctness.

### 3. Chunk size vs. reliability (empirically tuned, not fully understood)
Direct A/B evidence this session:
- 500-byte chunks: `notify()` reports `SUCCESS_NOTIFY` but the ack **never** arrives, even after
  5 retries over ~4 seconds. Reproducible, not a fluke.
- ~19–27-byte chunks (originally hit by accident via an MTU-lookup bug, see below): 100+
  consecutive chunks delivered with zero retries needed.
- 100-byte chunks (current setting, `BLE_CHUNK_PAYLOAD_SIZE` in the `.ino`): reliable in testing
  after fix #4 below (WiFi paused during BLE), including one clean multi-hundred-chunk transfer.

Leading theory: WiFi/BLE radio coexistence. This firmware runs WiFi STA + a `WebServer` alongside
BLE; larger BLE packets take longer on-air and are more exposed to coexistence contention than
tiny ones. Not independently proven beyond the correlation with fix #4 improving things.

Also worth knowing: an attempt to *derive* chunk size from the peer's negotiated MTU via
`BLEServer::getPeerDevices()/getPeerMTU()` was tried and reverted — this library version's
per-connection MTU bookkeeping proved unreliable (returned the un-negotiated 23B default long
after the app's `requestMtu(512)` had already succeeded), which is *how* the 19-27B chunk size
was originally (accidentally) discovered. `BLE_CHUNK_PAYLOAD_SIZE` is now a plain documented
constant (100) — do not resurrect the dynamic MTU lookup without re-verifying it against this
library version first.

### 4. WiFi/BLE coexistence — WiFi now paused during BLE sessions (implemented)
Per user decision: WiFi STA + the web server aren't needed at the same time as a BLE session in
real usage, so WiFi is now fully paused while a BLE client is connected, freeing the radio for
BLE exclusively:
- On BLE connect → disconnect: `WiFi.mode(WIFI_STA); WiFi.begin(); server.begin();`
- On BLE disconnect → ... : `server.stop(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF);`

**Important gotcha already hit once:** this logic must run on the **main loop task**, not inside
`MyServerCallbacks::onConnect()/onDisconnect()` (which run on the BLE stack's own task). Doing
the WiFi reconfiguration directly in the BLE callback caused `get_profiles`'s JSON response to
come back completely empty (`{"status":"ok","profiles":}`, 27 bytes) — reproducible instantly on
a fresh boot. Moving the actual `WiFi.*`/`server.*` calls into the existing
`deviceConnected`/`oldDeviceConnected` transition-detection block in the main `loop()` (a pattern
that already existed in this file for BLE-advertising-restart) fixed that specific corruption.
The BLE callbacks now only set `deviceConnected` and log; they do nothing else.

### 5. Two other, smaller correctness fixes landed alongside the above
(code-reviewed by an external reviewer, verified against the actual `flutter_blue_plus` 2.3.10
source before accepting)
- **Scan race in `connectBluetooth()`:** `await FlutterBluePlus.startScan(timeout: ...)` returns
  as soon as the platform scan call is issued, not when scanning actually finishes — verified by
  reading the package source (`_scanTimeout = Timer(timeout, stopScan)` is fire-and-forget). The
  original code then immediately cancelled its results subscription, which only "worked" because
  this device was scanned for repeatedly all session (Android scan cache hit). Fixed by awaiting
  either a `Completer` that fires when the target device is found, or `FlutterBluePlus.isScanning`
  emitting `false`.
- **Listener attach ordering:** `txChar.onValueReceived.listen(...)` is now attached *before*
  `setNotifyValue(true)`, not after — avoids a window where an early notification could be missed
  by the broadcast stream.
- **`bleBuffer` cleared on BLE disconnect** (firmware) so a half-received command from a dropped
  connection can't poison the next one.

## Outstanding, unresolved issue: heap/memory corruption in `get_profiles`

After fix #4, a **new** failure pattern appeared, confirmed across two independent long test
captures:

- The first one or two `get_profiles` calls in a boot session return the full, correct
  ~11–12KB JSON payload (verified fully delivered over BLE, chunk-by-chunk, with `BLE TX: sent`).
- Every `get_profiles` call *after* that returns exactly `{"status":"ok","profiles":}` — length
  27, i.e. the `profiles` value is completely empty (not even `[]` or `null`), for the rest of
  that boot session. A fresh reboot temporarily fixes it (gets 1-2 good fetches again, then
  degrades again).

This is very likely a heap fragmentation/exhaustion issue in `handleBleCommand()`'s `get_profiles`
branch (`M5_M6_config.ino`), which builds a fresh `String profilesJson` + `String response`
(~11-12KB) from `serializeJson(profilesDoc, profilesJson)` on every call. The ESP32-S3 here has
**no PSRAM** (confirmed in `CLAUDE.md`), so heap is limited, and there's already a code comment in
this exact function documenting an *earlier* OOM problem the developers hit and partially worked
around (by reusing the already-loaded global `profilesDoc` instead of re-parsing profiles.json
into a second `JsonDocument`). That workaround is evidently not sufficient once a couple of large
String builds have fragmented the heap.

**This is a real, separate bug from the BLE transport issue** — confirmed the BLE transport
itself is not at fault (it faithfully delivers whatever the firmware hands it, including this
malformed short payload).

### UPDATE 2026-07-16: fix implemented (compile-verified, not yet device-tested)

Suggested step 2 below was implemented, taken all the way to zero intermediate buffering:

- **`BleChunkSink` (Print subclass, in the `.ino`)**: `serializeJson(profilesDoc, sink)` now
  streams directly into the existing `sendNotifyAndWaitAck()` seq/ack pipeline through a fixed
  100-byte chunk buffer. The ~12KB payload never exists in RAM — peak heap cost of a fetch drops
  from ~24KB contiguous (two Strings held simultaneously, one pinned for the whole multi-second
  send) to effectively zero. `icon_xbm` pairs are stripped on the fly by a byte-level state
  machine in the sink (also removes the `String::remove()` churn from step 3). Wire format is
  byte-identical to the old String path — no app changes needed.
- **Step 1 instrumentation added**: `get_profiles` logs `ESP.getFreeHeap()` **and**
  `ESP.getMaxAllocHeap()` before/after each send. Watch `largestBlock`, not free heap — the
  exactly-empty (never partial) payload signature points to a failed up-front large allocation,
  i.e. largest-contiguous-block collapse, while total free heap likely still looks fine.
- **Bonus (RX-side mirror of the same problem)**: `handleBleCommand` now takes `char*` and uses
  ArduinoJson's zero-copy in-place parse, removing a full ~12KB copy during `save_profiles`
  (previously: strdup'd buffer + String copy + parsed doc all live at once).

Compile-verified with `scripts/build_fw.ps1` (wraps the documented FQBN): exit 0, flash 49%,
static RAM 26%.

### UPDATE 2026-07-17: device-tested, fix confirmed

Flashed to hardware and exercised over BLE from the companion app (freshly rebuilt debug APK,
same session): **5 consecutive `get_profiles` fetches in a single boot session**, spanning
~23:52–00:04 (~12 min), zero reboots in between. Every fetch returned the full, correctly-parsed
12037-byte payload (`Parse OK: status=ok`) — confirmed via `adb logcat` on the phone side. Zero
occurrences of the old 27-byte-empty-profiles corruption signature. This clears the bar the
"suggested next steps" section set (3+ consecutive calls, no degradation).

Also captured one live firmware-side heap snapshot mid-fetch (see tooling gotcha below for how):
`get_profiles: heap before: free=58892 largestBlock=24564` — healthy, non-fragmented, and that
fetch completed successfully immediately after. Didn't manage to capture a *second* in-session
heap snapshot for a direct before/after trend line (the serial capture tool proved flaky under
sustained load — see below), but the logcat evidence alone (5/5 full-size payloads, no corruption)
is sufficient to consider the streaming-sink fix verified.

**New tooling gotcha found this session:** capturing `Serial` output from this board while it's
running (not in the ROM bootloader) needs care with DTR/RTS, and it's *different* from the
bootloader-port behavior documented below:
- The running-mode port enumerates as `M5Stack "Dial"` (TinyUSB CDC), a **different port number**
  than the ROM bootloader port (`Espressif "USB JTAG/serial debug unit"`) — always re-check
  `arduino-cli board list` after flashing/reflashing/replugging.
- Opening the port with **both DTR and RTS asserted simultaneously** (as a naive
  "just enable everything" first attempt did) reliably reset the chip into the ROM bootloader's
  silent wait-for-flasher loop — no further app output, ever, until a physical unplug/replug.
  Likely cause: the transition edge on RTS reads as a reset pulse, and DTR-high-at-that-instant
  reads as the GPIO0/boot-select line, same as `esptool`'s bootloader-entry dance — even though
  this is the app's own TinyUSB CDC, not the ROM's converter.
- With **both DTR and RTS deasserted**, the port stays open and stable (no unwanted reset) but
  **zero bytes are ever received**, even during confirmed BLE activity — this core's USB-CDC
  appears to gate transmit on the DTR "terminal open" line state, like classic Arduino
  Leonardo/Micro behavior.
- **DTR asserted, RTS deasserted** is the combination that actually works: stable connection, no
  reset, and real data flows. `.NET`'s `System.IO.Ports.SerialPort` was unreliable even with this
  correct combination (`"the port is closed"` errors, and separately, silent stalls after ~30
  chunks under sustained BLE-transfer load with no exception raised). **Python + `pyserial`** with
  the same DTR/RTS combination was solid for opening the connection, though it also silently
  stopped receiving bytes partway through one long transfer (~32 of ~121 chunks) without erroring
  — worth another look if firmware-side capture is needed again, but not investigated further
  this session since the logcat evidence was already conclusive.
- New reusable script: `scripts/serial_capture.ps1` (PowerShell/.NET version, documents the
  DTR=true/RTS=false finding in a comment) — treat its data as best-effort, not fully reliable
  under load; a `pyserial`-based capture is likely a better foundation if this needs revisiting.

### Suggested next steps (from before the fix above; step 1 and 2 now done)
1. Add `Serial.println(ESP.getFreeHeap())` immediately before and after the `serializeJson()` /
   `String` concatenation in the `get_profiles` branch, across several consecutive calls in one
   boot session, to directly observe whether free heap is trending down and how much is available
   when the corruption starts.
2. Consider avoiding the double-buffering: `serializeJson` into `profilesJson`, then
   `String response = prefix + profilesJson + suffix` effectively holds ~2x the payload size in
   heap simultaneously (plus String reallocation overhead as it grows). Serializing directly into
   one pre-sized buffer (or streaming chunks straight out of `serializeJson` via a custom
   `Print`-like sink that also does the chunked-BLE-send, skipping the intermediate `String`
   entirely) would meaningfully cut peak heap usage.
3. Check whether `icon_xbm` stripping (`profilesJson.remove(idx, STRIP_LEN)` in a loop) is
   itself expensive/fragmenting on a `String` this large — repeated in-place `String::remove()`
   calls on an ~11KB `String` can cause internal reallocation churn.
4. If heap is confirmed low/fragmented, consider whether other long-lived allocations earlier in
   the session (BLE stack buffers, WiFi buffers before it's turned off, etc.) are the real
   culprits rather than `get_profiles` itself — the timing (degrades after ~2 calls, not
   immediately) suggests cumulative fragmentation rather than a single-call leak.

## UPDATE 2026-07-17 (later): `save_profiles` RX corruption — found, fixed, and verified

Discovered while testing the `get_profiles` heap fix above via the companion app's UI: renaming
a profile (any `save_profiles` call, i.e. the RX/upload direction — app to firmware) reliably
failed with `{"status":"error","message":"Invalid JSON"}`, even with the profile name unchanged.
This is a **separate bug from everything above**, on the opposite data direction.

### Root cause (two compounding issues)

1. **`onWrite()` fires twice per actual BLE write from the app.** Confirmed directly: for a
   ~12KB `save_profiles` upload split into ~68 app-side chunks, the firmware's serial log showed
   `BLE RX bytes: N` printed exactly twice per chunk, every time, for the whole transfer (136
   total logged writes for 68 real ones). Same underlying quirk as the already-known doubled
   `SUCCESS_NOTIFY` callback on the TX side (see fix #2 above) — benign there since it's just a
   log line, **not benign here** since it used to double-append into the RX reassembly buffer.
2. **The RX reassembly buffer (`bleBuffer`, an Arduino `String` grown one byte at a time via
   `+=`) silently corrupts under heap pressure** — the same class of bug as the `get_profiles`
   fix above, just on the receive side. Direct evidence: heap readings taken mid-session showed
   `largestBlock` as low as ~9KB, well under the ~12-24KB (doubled) the buffer needed to grow to.
   Combined with issue #1 doubling the growth rate, nearly every append silently failed, and only
   a short tail fragment (e.g. `handleBleCommand: color":"#FF00FF"}]}}`) ever made it into the
   buffer by the time the terminating `\n` was found — hence `deserializeJson()` throwing
   `InvalidInput` on a JSON fragment instead of the real command.

Confirmed **not** a data-loss risk on the stored profile: a failed `save_profiles` returns early
(before `LittleFS` write) and a follow-up `get_profiles` reliably still showed the old, unmodified
profile — the bug corrupts the in-flight command, not the on-disk file.

### Fix implemented (compile- and device-verified)

- **Duplicate-write suppression** in `MyCallbacks::onWrite()`: an exact repeat of the
  immediately-previous write's bytes is now dropped (logged as `BLE RX: duplicate write
  ignored`) before it reaches the reassembly buffer.
- **RX reassembly buffer is no longer an incrementally-grown `String`.** Replaced with a fixed
  `BLE_RX_BUFFER_SIZE` (16384 bytes) `char*` buffer, filled with simple bounds-checked indexing
  (`bleRxBuf[bleRxLen++] = c`) instead of `String::operator+=`.

**Important gotcha hit while building this fix — do not repeat:** the RX buffer must **not** be a
compile-time-sized static/global array (`static char bleRxBuf[24576];`). That was the first
implementation, and it compiled fine (33% static RAM, plenty of reported headroom) but **silently
broke BLE advertising** — the device would boot, run its display loop completely normally (looked
fine to a human glancing at the screen), but never appear in a BLE scan from the app again, with
zero serial output of any kind (no crash, no error, nothing) even across multiple full power
cycles and phone-side Bluetooth toggles. Confirmed by direct isolation: shrinking the static
array to 2KB immediately restored BLE connectivity; growing it back broke it again. Root cause
not fully diagnosed (something in `BLEDevice::init()`'s own allocations apparently needs headroom
the compile-time RAM report doesn't capture as a hard constraint), but the fix is straightforward
and more correct anyway: `bleRxBuf` is now `malloc()`'d **once**, in `setup()`, immediately after
`BLEDevice::init()` — a single clean heap allocation on a still-fresh, unfragmented heap, not a
permanent static reservation competing with BLE bring-up. Logs `FATAL: failed to allocate BLE RX
buffer` and no-ops in `onWrite()` if the allocation itself ever fails.

Device-verified end to end: renamed a profile via the companion app UI (BLE), `save_profiles`
returned `{"status":"ok"}`, and the new name persisted through a `get_profiles` re-fetch and was
reflected live in the UI.

## Current firmware constants/behavior to know about

- `BLE_CHUNK_PAYLOAD_SIZE = 100` (bytes, excluding the 1-byte sequence prefix) — a documented,
  hardcoded compromise value; not derived from MTU (see #3 above for why).
- `sendNotifyAndWaitAck()` retries up to 5 times per chunk, ~800ms per attempt.
- WiFi/web server are off for the full duration of any BLE connection; restored on disconnect.
- Diagnostic `Serial` logging left in place intentionally (`BLE TX: notify seq=...`,
  `BLE TX notify status [core=... task=... us=...]: ...`, `BLE TX: connectedCount=... peerDevices=...`)
  — useful for the heap investigation above; safe to trim once the memory issue is resolved.

## Tooling notes learned this session (useful for future debugging)

- `arduino-cli monitor` does not work for automated/background serial capture in this
  environment — it treats non-interactive stdin as an immediate quit signal and exits within the
  same second it connects, regardless of the requested duration. Use a direct `.NET SerialPort`
  script (PowerShell) instead — set `DtrEnable`/`RtsEnable` true, poll `ReadLine()` with a short
  `ReadTimeout` in a loop, per-line `Add-Content` to a log file so partial output survives even if
  the capture is killed mid-way.
- The M5Dial enumerates as a **different COM port** depending on mode: the JTAG/download-mode
  port (Espressif "USB JTAG/serial debug unit") vs. the normal running-mode port (M5Stack "Dial",
  TinyUSB CDC) — always re-check `arduino-cli board list --format json` after any mode change
  before assuming a port is still valid.
- The full FQBN needed for this firmware: `` `m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB` `` (also documented in
  `docs/Toolchain_arduino-cli.md`).
- `flutter install` does **not** rebuild the app — it only installs whatever APK already exists
  in `build/app/outputs/flutter-apk/`. Always run `flutter build apk --debug` (or `--release`)
  explicitly first when testing a Dart-side change, and check for the `Running Gradle task
  'assembleDebug'... <N>s` line to confirm a real build happened, not a stale-APK reinstall.
- Debug builds are required to see the companion app's own `debugPrint()`/`print()` output via
  `adb logcat` — release builds do not forward Dart stdout to logcat at all.
