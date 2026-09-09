# RESOLVED — Flutter "Toolchain Issue" on Windows

*Raised 2026-07-25. **Resolved same day: it was not a toolchain problem.***

> **Do not uninstall or reinstall Visual Studio.** The original version of this document
> recommended a full VS reinstall. That would have cost hours and fixed nothing, because Visual
> Studio was never required for this project.

---

## The actual answer

The blocker was **targeting the wrong platform.** The companion app was being built with
`flutter run -d windows`, which requires the Visual Studio C++ toolchain.

**The Draupnir companion app is an Android app.** It always has been — the screenshots in
`companion_app/Screenshots/` are Android, and the project's tooling notes are entirely about
`flutter build apk`, `flutter install`, and `adb logcat`. The `companion_app/windows/` directory
exists only because `flutter create` scaffolds every platform by default. Nothing targets it.

`flutter doctor` shows the split clearly:

```
[√] Android toolchain - develop for Android devices (Android SDK version 36.0.0)
[!] Visual Studio - develop Windows apps        <- only this is broken, and only this
```

**Android is green.** Verified end-to-end on 2026-07-25:

```
$ flutter build apk --debug
Running Gradle task 'assembleDebug'...      48.0s
√ Built build\app\outputs\flutter-apk\app-debug.apk
```

The Visual Studio warning is real but **irrelevant**. Leave it. Only fix it if you specifically
want a Windows desktop build for its own sake, which nothing in this project needs.

---

## Why a desktop build was the wrong direction anyway

The goal was the M6 H2/H3 functional tests. Those need a **phone**:

- The H1 negative test (`docs/M6_HardeningWorkOrder.md`) requires nRF Connect or equivalent on a
  phone to attempt an unbonded write.
- BLE pairing is a passkey-entry flow through the phone's Bluetooth settings.

So there is **no route through M6 that avoids the phone.** Time spent on a Windows desktop build
was time spent away from the only device that can finish the milestone.

---

## The `firmware_tests.dart` workaround is also a dead end

`companion_app/test/firmware_tests.dart` was written to drive the tests headlessly. It correctly
failed under `flutter test` (plugins need a platform), but **fixing the toolchain would not have
saved it.** Four independent problems:

1. **It omits ACKs — fatal.** The script notes *"ACK sending logic omitted for simplicity."* The
   firmware's `sendNotifyAndWaitAck()` blocks on a `[0xFE, seq]` echo per chunk, retries 5 × 800 ms,
   then aborts the message. Without acks the first `get_profiles` never completes.
2. **`trigger` semantics are wrong.** The firmware reads `req["macro"]` as a **`pos`** and ignores
   `req["profile"]` entirely (`ble_engine.cpp:176-185`). The script passes array indices. It works
   by coincidence on the default profile (pos 0-4 == indices 0-4) and breaks on any reorder.
3. **Completer race.** `trigger` is sent without `await`, then `save_profiles` immediately
   overwrites `_bleResponseCompleter` and clears `_bleBuffer`. For a deliberately timing-sensitive
   test, this makes results meaningless.
4. **`runH2Test()` is an unimplemented stub.**

The file is untracked and **recommended for deletion.** If a headless harness is ever wanted, it
needs the full ack protocol and `integration_test` on a real device — not `flutter test`.

---

## Do this instead

Both tests are ordinary UI actions in the existing app, which already implements the ack protocol
correctly:

```
cd companion_app
flutter build apk --debug     # ALWAYS build first -- `flutter install` does NOT rebuild
flutter install
```

Confirm the `Running Gradle task 'assembleDebug'... <N>s` line actually appears; a suspiciously
fast install means a stale APK was reused.

- **H3** — fire the "Caps Lock" macro so the toggle loops, then Save in the app. Pre-H3 this was a
  guaranteed use-after-free. Watch serial for a reset.
- **H2** — build and save a profile with 16 macros with icons. Must succeed where the old 4 KB
  buffer would have wedged the channel.

Capture serial throughout with `scripts/serial_capture.ps1` (**not** `arduino-cli monitor`).

> **Note on which firmware is flashed.** The device currently runs `a2733c7` — **H2–H7 only**. H1
> is not on it, so the app works unpaired. Once H1 (`3607a28`) is flashed, the app needs its H1
> changes too; those exist in that same commit and have never been run.

---

## Environment note (minor, unrelated)

`flutter doctor` also warns that `dart` on PATH resolves to a winget-installed Dart SDK rather than
the one inside `C:\Users\ido11\flutter`. It is not currently breaking anything, but it is a real
mismatch. If odd Dart behaviour ever appears, put `C:\Users\ido11\flutter\bin` at the front of PATH.
