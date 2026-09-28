# M13 — OTA firmware update from the Companion App

**Status:** design approved 2026-09-28. Implementation not started.
**Roadmap entry:** `docs/Draupnir_Spec.md` §10, M13.

---

## 1. Why

Draupnir ships as a self-contained USB-HID knob with no host software. Today the only way to change
its firmware is a USB cable, a held BOOT button, and `arduino-cli` — which is fine for the author
and impossible for anyone else. Without OTA the product is not maintainable in the field: a bug
found after shipping stays shipped.

It is also what makes **M12's design affordable in the long run.** M12 compiles its glyph set into
the firmware and lets the app fill gaps with a bitmap, on the explicit reasoning that a firmware
release merely *promotes* an icon from "sent as an image" to "free and crisp". That trade is only
comfortable if shipping firmware is routine. M13 is what makes it routine.

## 2. Decisions taken before designing

### 2.1 The image ships inside the app, with a seam for later

The `.bin` is a Flutter asset. Updating a knob becomes: update the app from Play, open it, it
offers the newer firmware.

**No `INTERNET` permission.** The release manifest does not declare it, and the Play Data
Safety declaration reads "collects no data, shares no data" on that basis — the manifest says so
in a comment at the top, and `grep -c "uses-permission.*INTERNET"` returns 0.

A download-from-URL design would have to restore that permission, add TLS and availability
concerns, and widen the network surface weeks after a security gate — for a benefit (shipping
firmware without a Play review) that does not yet matter.

One trap for whoever builds the URL variant later: **debug and profile builds DO get `INTERNET`**
from `src/debug` and `src/profile`, because the Flutter tool needs it for hot reload. So a download
path would work perfectly in every development build and fail only in release. Test that in a
release build or not at all.

The app side goes behind `abstract class FirmwareSource`; `BundledFirmwareSource` is the only
implementation. A future `UrlFirmwareSource` is an app-side swap that the device never sees,
because the device is told nothing about where the bytes came from.

### 2.2 Images are signed; the public key is compiled in

**Threat model, stated plainly.** This device is a USB HID keyboard. Hostile firmware is a
keylogger and a keystroke injector on whatever host it is plugged into. To push one over BLE an
attacker must already be **bonded**, which requires physical access to read the passkey off the
screen — and anyone with physical access can hold BOOT and flash over USB anyway. So a signature
does **not** defend against physical access, and is not meant to.

What it defends against is **a previously-bonded phone turning hostile**: malware on the owner's
own device pushing a keylogger to a knob it is already trusted to configure. That is a real and
narrow threat, and a signature raises the bar from "be bonded" to "hold the private key".

**Secure Boot v2 was rejected** for this stage. It is the only option that also resists physical
reflashing, but the eFuse burn is irreversible, a mistake bricks a board permanently, and it
complicates every development flash from here on. That is a one-way door to walk through
deliberately, if ever, not as part of adding OTA.

**Consequences that need owning:**

- The private key **cannot live in this repo**, which is MIT and public.
- **Losing the key ends OTA for every shipped unit.** It needs a real home and a backup before
  the first release, not after.
- Anyone can still build and flash their own firmware over USB. The signature gates the wireless
  path only; it never locks an owner out of their own hardware. For an open-source device that is
  the point, not a gap.

### 2.3 Binary mode on the existing characteristic

The image is ~1.18 MB. The existing command channel is JSON over **100-byte** chunks with an ack
per chunk — 11,773 round trips, four to ten minutes, and the wrong shape for binary entirely.

Note the waste this exposes: the app already negotiates `requestMtu(512)` and logs a 509-byte
payload, but `BLE_CHUNK_PAYLOAD_SIZE` is hardcoded to 100. Every chunk uses a fifth of the packet
it paid for. That slows profile fetches today; OTA is merely what makes it intolerable.

**A dedicated OTA characteristic was rejected.** It would be faster still (write-without-response,
several packets per connection interval, plausibly 20-30 s), but it is a brand-new GATT surface
that must be given encryption and authentication flags matching RX's exactly. Getting that wrong
silently reopens the hole that the M6 and M5Dial security gates closed — and those gates were
proven by *refusal*, with a hostile-central test, precisely because this class of mistake compiles
cleanly and enforces nothing. Reusing RX inherits its flags rather than reproducing them.

## 3. Architecture

### 3.1 Protocol

Four new JSON commands on the existing RX characteristic, plus a binary mode between two of them.

**`ota_begin`**

```json
{"cmd":"ota_begin","version":"...","size":1177270,
 "sha256":"<64 hex>","sig":"<hex DER ECDSA-P256>"}
```

The device validates the request, calls `esp_ota_begin()` on the inactive partition, and replies:

```json
{"status":"ok","offset":0}
```

`offset` is where the app should start sending. It is non-zero only when resuming within the same
session (§3.4). On refusal the device replies `{"status":"error","message":"..."}` and stays in
JSON mode.

Refusal cases, all before any partition write: image larger than the partition; malformed hash or
signature; a macro currently running that could not be stopped; already in an OTA.

**Binary mode.** After a successful `ota_begin` the RX characteristic carries **raw image bytes**
— no JSON, no framing, no base64. 509-byte payloads at the negotiated MTU. The device writes each
into the OTA partition and folds it into a running SHA-256.

Every **16 chunks** (~8 KB) the device notifies an ack carrying the byte offset it has accepted,
which is what lets the app pipeline rather than wait per chunk, and what gives resume its
anchor.

**`ota_end`** — sent as JSON once all bytes are delivered. The device:

1. checks the streamed SHA-256 against the one from `ota_begin`;
2. verifies the ECDSA-P256 signature over that digest against the compiled-in public key;
3. **only then** calls `esp_ota_end()` and `esp_ota_set_boot_partition()`;
4. replies, then reboots.

Order matters: a corrupt or forged image never becomes bootable at all.

**`ota_abort`** — releases the handle via `esp_ota_abort()` and returns to JSON mode. An
inactivity timeout in binary mode does the same thing unprompted, so a dropped transfer cannot
strand the device in a mode where it no longer understands commands.

**`ota_confirm`** — see §3.3.

### 3.2 Signing

- **ECDSA P-256** over the SHA-256 of the image. `CONFIG_MBEDTLS_ECDSA_C=y` and
  `CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED=y` are already set in the Arduino core's sdkconfig, so
  no toolchain change is needed.
- Public key compiled into the firmware as a byte array.
- `tools/sign_firmware.py` reads the private key from an environment variable and emits the
  manifest the app ships. The key path is never a default, never a repo path, and the script
  fails loudly rather than silently signing with something it found lying around.

### 3.3 Two independent safety nets

**Net one — verify before committing.** §3.1's ordering. Defends against corruption, truncation,
and forgery.

**Net two — rollback.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is already set in the Arduino
core. The new image boots in `ESP_OTA_IMG_PENDING_VERIFY`; if it never confirms itself, the
bootloader reverts to the previous partition on the next power cycle.

**What counts as confirmation is the load-bearing choice.** The firmware does **not** confirm
itself on boot. It waits for the app to reconnect and send `ota_confirm`, and only then calls
`esp_ota_mark_app_valid_cancel_rollback()`.

That is deliberate. Confirmation then requires the device to boot, initialise BLE, advertise,
accept a bonded connection, and serve a command — which is exactly the capability that would be
lost if the new image were broken. A firmware that cannot do those things cannot confirm itself,
and is reverted automatically. Self-confirming on boot would mark a half-working image valid and
throw away the net.

### 3.4 Resume, scoped honestly

`esp_ota_write` is sequential and the handle does not survive a reboot. So:

- **Reconnect mid-transfer:** supported. The device keeps the handle alive under the inactivity
  timeout; `ota_begin` with a matching `sha256` replies with the offset already accepted and the
  app seeks.
- **Device reboot mid-transfer:** starts over. Persisting partition write state to survive a
  reboot is real work for a transfer that takes about a minute.

### 3.5 Device behaviour during an update

- **Stop the macro engine first.** It is loop()-task only and holds `JsonObject` references into
  the profile document (`CLAUDE.md`, threading rules). `ota_begin` refuses if macros cannot be
  stopped.
- **Quiesce HID.** A half-flashed device must not be able to type into the host.
- **Show progress on the ring.** LVGL keeps running; the draw path is unaffected by partition
  writes.
- **Partition writes happen on the loop() task**, never on the BLE host task or the LVGL task.

## 4. App behaviour

- Compares the bundled manifest version against the device's reported firmware version and offers
  the update; does not auto-apply.
- Shows progress and a clear instruction not to unplug.
- On completion: waits for the reboot, reconnects, sends `ota_confirm`, and reports success only
  after that round trip. Until `ota_confirm` succeeds the update is not done — it is on probation.
- **Downgrades are allowed, with a warning.** Refusing them would make a bad release recoverable
  only over USB, which is exactly the situation OTA exists to avoid.

## 5. Risks

**Key management is now an operational concern**, not a coding one. See §2.2.

**The `ota_confirm` round trip can strand a working image.** If the new firmware boots fine but
the user never reopens the app, the next power cycle reverts a good update. Mitigation: the app
prompts to reconnect immediately, and the window is bounded by the user's next launch. A
time-based or boot-count self-confirm would remove the net's whole value (§3.3), so the awkwardness
is accepted rather than designed away.

**Binary mode is a mode**, and modes strand things. The inactivity timeout is the guard; it must be
tested by actually dropping a transfer, not by reading the code.

**A partially-written partition is safe but wastes a slot** until the next `ota_begin` overwrites
it. No cleanup is needed; noting it so nobody invents some.

**Flash wear** is not a practical concern at any plausible update cadence.

## 6. Out of scope

- Downloading images from a URL (§2.1 keeps the seam).
- Secure Boot v2 and any eFuse burning (§2.2).
- OTA for the retired M5Dial. It is frozen; it will not receive this or anything else.
- Delta/differential updates. At ~1 minute for a full image the complexity earns nothing.
- Persisting resume state across a device reboot (§3.4).
- Raising `BLE_CHUNK_PAYLOAD_SIZE` for the existing JSON path. §2.3 notes the waste; changing the
  shared chunk size touches a protocol the frozen M5Dial also speaks, and that is its own change
  with its own compatibility argument. Binary mode sets its own payload size and does not disturb
  it.

## 7. Done criteria

1. `tools/sign_firmware.py` signs an image and fails loudly when the key env var is unset.
2. A tampered image — one byte flipped after signing — is **refused**, and the device remains on
   its existing firmware.
3. An image signed with the wrong key is refused.
4. A truncated transfer (disconnect mid-flight) leaves the device working, on its old firmware,
   and back in JSON mode after the inactivity timeout.
5. A good update completes, reboots, reconnects, and is confirmed via `ota_confirm`.
6. **The rollback net is proven by actually using it**, not by inspection: install an image that
   boots but deliberately never gets confirmed, power-cycle, and observe the device return to the
   previous firmware.
7. Macros are stopped and HID is quiesced for the duration; the host receives no keystrokes.
8. Transfer of the real ~1.18 MB image completes in roughly a minute.
9. Resume-after-reconnect works mid-transfer.
10. A downgrade is offered with a warning and installs correctly.
11. The retired M5Dial, unflashed, is unaffected — it never sees these commands and continues to
    work.
