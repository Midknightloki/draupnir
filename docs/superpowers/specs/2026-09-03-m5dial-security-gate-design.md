# M5Dial Security Gate — Design

*Written 2026-09-03. Target: `firmware/M5_M6_config/M5_M6_config.ino` + `companion_app/`.*

## Goal

Close the M5Dial's config channel. Today it has **no cryptographic access control at all** — no
BLE pairing, no bonding, no GATT permission flags — and it additionally exposes a LAN-reachable
HTTP API that can inject keystrokes into the attached host. This milestone brings the M5Dial to
the same security posture the Waveshare knob reached in M6/H1, and removes the Wi-Fi surface
permanently.

The M5Dial is a USB HID keyboard. Every hole here is a remote keystroke-injection hole.

## What is actually wrong today

Three distinct problems, in descending order of severity:

**1. The web server (worst).** `WebServer server(80)` runs in `RUN_MODE` and `CONFIG_MODE`.
`POST /api/profiles` rewrites the macro set; `POST /api/trigger` fires a macro — i.e. types into
the host. `isAuthorized()` (line 1188) returns nothing more than "are we in Config Mode". Anything
on the LAN can reach it, with no proximity requirement and no user-visible signal. mDNS even
advertises it as `draupnir.local`.

**2. No BLE security.** The BLE config channel has no `BLESecurity` block, no passkey, and no
permission flags on RX/TX. Any central in radio range can write `save_profiles` or `trigger`.
This is the gap the file's own header comment (lines 34–59) flags as "the top item of the M5Dial
catch-up".

**3. The Config Mode gate is not a security control.** `handleBleCommand` refuses everything
outside `CONFIG_MODE` (line 933). That is a *physical presence* check, not authentication: it
stops nothing once the user swipes down, and it is the only thing standing between an arbitrary
central and the host's keyboard.

Note what this means for the current state: the M5Dial's protection is **entirely** items that
are about to be deleted or replaced. There is no interim posture to preserve.

## Non-goals

- No application-layer auth token. The `pairingToken` / `pair` scheme was removed in M6 and is
  **not** coming back; the rebuttal is in the sketch header and `docs/HANDOFF.md` §8.
- No Wi-Fi, in any form, for any reason. Cut, not deferred (`CLAUDE.md`, spec v3 §1/§7).
- No M5Dial UI rework beyond the passkey screen. That is M5Dial catch-up work, later.
- No negative (hostile-central) hardware test this round — see **Verification**.

---

## Move 1 — Wi-Fi and the web server come out

Delete, in `M5_M6_config.ino`:

| What | Where |
|---|---|
| `#include <WiFi.h>`, `<WiFiManager.h>`, `<ESPmDNS.h>`, `<WebServer.h>`, `"index_html.h"` | 5–8, 13 |
| `WIFI_SETUP_MODE` from `enum AppMode` | 32 |
| `WebServer server(80);` | 101 |
| `bool mdnsStarted` | 107 |
| `isAuthorized()` | 1188 |
| `setupWebServer()` and all four routes | 1192–1291 |
| `enterWiFiSetupMode()` | 1314–1341 |
| `WiFi.mode(WIFI_STA); WiFi.begin(); setupWebServer();` in `setup()` | 1382–1385 |
| the mDNS/`WiFi.status()` block in `loop()` | 1453–1460 |
| the `BtnA.wasHold()` → `enterWiFiSetupMode()` trigger | 1512–1516 |
| `bool waitRelease` and its three uses | 1440, 1514, 1587–1591 |
| all three `server.handleClient()` calls | 1450, 1577, 1585 |
| the `WIFI_SETUP_MODE` branch of `loop()` | 1584–1593 |

Delete the file `firmware/M5_M6_config/index_html.h`.

Remove `WiFiManager` from the build's library requirements (documentation only — there is no
manifest in this tree).

**The one trap.** The Wi-Fi pause/restore blocks at 1605–1626 are being deleted, and they
*contain* the BLE reconnection logic:

```c
  if (!deviceConnected && oldDeviceConnected) {
    WiFi.mode(WIFI_STA); WiFi.begin(); server.begin();   // <- goes
    delay(500);                                          // <- stays
    BLEDevice::startAdvertising();                       // <- STAYS
    oldDeviceConnected = deviceConnected;                // <- STAYS
  }
  if (deviceConnected && !oldDeviceConnected) {
    server.stop(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF);  // <- goes
    oldDeviceConnected = deviceConnected;                       // <- STAYS
  }
```

Delete the Wi-Fi lines and keep the rest. Losing `startAdvertising()` means the board never
re-advertises after a disconnect — it looks like a dead radio, and it is exactly the kind of
regression a positive-only test round would catch late. This is criterion 1 in Verification.

The second block's body becomes only `oldDeviceConnected = deviceConnected;`. Keep the `if` as-is
rather than collapsing the two blocks; the transition pair is a recognisable idiom and the
disconnect side still does real work.

**Config Mode screen.** `enterConfigMode()` prints `WiFi.localIP()` at line 1308. Replace the
IP with the BLE identity and link state:

```c
  d.drawString("BLE", 120, 100);
  d.drawString("Draupnir_Mini", 120, 130);
  d.setTextColor(deviceConnected ? TFT_GREEN : TFT_DARKGRAY, TFT_BLACK);
  d.drawString(deviceConnected ? "app connected" : "waiting for app", 120, 160);
```

The screen keeps its "CONFIG MODE" title and "TAP TO EXIT" footer. After Move 2 the mode no
longer gates anything, but it remains a useful status screen and an explicit "I am talking to the
app now" affordance — the user chose *drop the gate, keep the mode*.

**A note on what this deletion costs.** The `server.stop()` on BLE connect was load-bearing for a
real bug: Wi-Fi/BLE coexistence was the leading suspect for large notify chunks being silently
dropped, and pausing Wi-Fi during a BLE session fixed it. Removing Wi-Fi entirely subsumes that
fix — the contention cannot recur if the radio is never brought up. The comment explaining the
history should be preserved in condensed form at the BLE init site so the reasoning is not lost
with the code.

---

## Move 2 — The BLE gate

Port from `firmware/Waveshare_LVGL_Test/ble_engine.cpp`, which is proven on hardware.

**Build-time guards**, at file scope just after the `#include <BLEDevice.h>` block (not inside
`setup()` — they must sit somewhere the include has already defined the symbols, and namespace
scope is the obvious place). These exist because the user chose positive-testing only; they
convert the failure mode this API is notorious for — compiling cleanly and enforcing nothing —
into a build error:

```c
#if !defined(CONFIG_NIMBLE_ENABLED) && !defined(CONFIG_BT_NIMBLE_ENABLED)
#error "Expected a NimBLE-backed core. Under Bluedroid, PROPERTY_*_ENC/_AUTHEN are defined as 0 \
and the GATT permission flags below would silently enforce nothing. See ble_engine.cpp."
#endif
static_assert(BLECharacteristic::PROPERTY_WRITE_ENC != 0,
              "PROPERTY_WRITE_ENC is 0 — the RX permission flags would be a no-op.");
static_assert(BLECharacteristic::PROPERTY_WRITE_AUTHEN != 0,
              "PROPERTY_WRITE_AUTHEN is 0 — the RX permission flags would be a no-op.");
```

All of this was checked against the installed headers rather than assumed
(`packages/m5stack/hardware/esp32/3.3.8/libraries/BLE/src/`), because on this core the wrong
answer compiles:

- `BLECharacteristic.h:162-194` defines `PROPERTY_WRITE_ENC` / `_AUTHEN` as **`0`** under
  `#if defined(CONFIG_BLUEDROID_ENABLED)` and as the real `BLE_GATT_CHR_F_*` bits under
  `#if defined(CONFIG_NIMBLE_ENABLED)`. They are `static const uint32_t` with in-class constant
  initializers, so `static_assert` on them is well-formed.
- m5stack:esp32 3.3.8 resolves `{runtime.tools.esp32s3-libs.path}` to the Espressif
  `esp32s3-libs` package, whose `sdkconfig.h` defines `CONFIG_BT_NIMBLE_ENABLED 1` and
  `CONFIG_NIMBLE_ENABLED` — so the NimBLE branch is the live one.
- `BLESecurity.h` declares every static used above (`setAuthenticationMode`, `setCapability`,
  `setInit/RespEncryptionKey`, `setPassKey`, `regenPassKeyOnConnect`, and the
  `startSecurity(uint16_t connHandle, int*)` overload), and `BLESecurityCallbacks` declares
  `onPassKeyRequest`, `onPassKeyNotify`, `onSecurityRequest`, `onConfirmPIN`, and **both**
  `onAuthenticationComplete` overloads — `esp_ble_auth_cmpl_t` and `ble_gap_conn_desc*`. Override
  the latter.
- `BLEServer.h:306-307` declares the `onConnect/onDisconnect(BLEServer*, ble_gap_conn_desc*)`
  overloads used below.
- `BLECharacteristic.h:245` stores properties as `esp_gatt_char_prop_t m_properties` — a
  `uint16_t` — which is the truncation behind the known gap noted further down.
- `BLE_HS_CONN_HANDLE_NONE` (`0xffff`) is defined in `host/ble_hs.h`, and `ble_gap_conn_find()`
  is declared in `host/ble_gap.h` alongside `struct ble_gap_conn_desc` — the same header that
  makes the callback overloads above compile, so if the types resolve the function does too. If
  the sketch turns out not to see them transitively through `BLEDevice.h`, add
  `#include <host/ble_gap.h>`; that is a loud compile error, not a silent one.

**Security setup**, in `setup()` immediately after `BLEDevice::setMTU(512)` and before
`createServer()`:

```c
  BLESecurity::setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  BLESecurity::setCapability(ESP_IO_CAP_OUT);
  BLESecurity::setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setPassKey(false, 0);
  BLESecurity::regenPassKeyOnConnect(true);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());
```

Carry the Waveshare's **rollback ladder** comment across verbatim: if unexplained resets into the
ROM bootloader appear, (1) flip `regenPassKeyOnConnect` to false, (2) drop `PROPERTY_WRITE_AUTHEN`
from RX, (3) drop the ENC flags entirely — with the explicit warning that stopping at step 3 is
insecure and the boot reason should be reported instead. That history is why the ladder exists;
the M5Dial has no reason to be immune.

**`SecurityCallbacks`**, ported as-is. `onPassKeyRequest()` → 0; `onSecurityRequest()` → true;
`onConfirmPIN()` → true; the NimBLE `onAuthenticationComplete(ble_gap_conn_desc *desc)` overload
logging `encrypted/authenticated/bonded`. `onPassKeyNotify()` sets `currentPasskey` and
`pairingActive` and **does not draw** — it runs on the BLE host task (see Move 4).

**`MyServerCallbacks::onConnect`** switches to the NimBLE overload so it has a connection handle:

```c
  void onConnect(BLEServer* pServer, ble_gap_conn_desc* desc) override {
    deviceConnected = true;
    int rc = 0;
    bool started = BLESecurity::startSecurity(desc->conn_handle, &rc);
    Serial.printf("BLE connected, conn_handle=%d startSecurity ok=%d rc=%d\n",
                  desc->conn_handle, started, rc);
  }
```

`onDisconnect` gains `pairingActive = false;` alongside the existing `bleRxLen = 0;`.

**Deliberately NOT ported:** `server->updateConnParams(desc->conn_handle, 0x28, 0x50, 4, 400)`.
That is a Waveshare-specific fix for USB HID reports being starved by a tight connection interval.
The M5Dial has not shown that symptom, and changing connection parameters as a side effect of a
security change would confound the verification round. If HID starvation appears on the M5Dial
later, port it then, on its own.

**Characteristics.** RX and TX get the enforcement bits, which under NimBLE live in the
*properties* bitmask — `setAccessPermissions()` is a silent no-op on this core:

```c
  pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_TX,
    BLECharacteristic::PROPERTY_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC);

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
      | BLECharacteristic::PROPERTY_WRITE_ENC | BLECharacteristic::PROPERTY_WRITE_AUTHEN);
```

Drop `pTxCharacteristic->addDescriptor(new BLE2902())` (line 1417) and the `#include <BLE2902.h>`.
NimBLE creates the CCCD itself and `addDescriptor()` early-returns under
`#ifdef CONFIG_NIMBLE_ENABLED` — it is dead code. The auto-created CCCD is protected by
`BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC` on the characteristic.

Port the **known-gap** comment too: `BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN` (0x10000) truncates
away because `m_properties` is a `uint16_t`, so the CCCD is gated on encryption but not explicitly
on authentication — safe only because `ESP_LE_AUTH_REQ_SC_MITM_BOND` refuses Just Works outright.
On the Waveshare that inference was upgraded to an observation by hardware test; on the M5Dial it
stays an inference this round. Say so in the comment rather than copying the Waveshare's
"VERIFIED" language across.

**Delete the Config Mode gate** at line 933 and replace it with the runtime backstop:

```c
  // Belt-and-braces behind the GATT permission flags on RX. If those flags ever silently fail
  // to enforce (the failure mode this API is known for — see the build guards at BLE init),
  // this refuses the command anyway and says so on Serial, instead of executing it quietly.
  ble_gap_conn_desc desc;
  if (bleConnHandle == BLE_HS_CONN_HANDLE_NONE ||
      ble_gap_conn_find(bleConnHandle, &desc) != 0 ||
      !desc.sec_state.encrypted || !desc.sec_state.authenticated) {
    Serial.println("BLE cmd REFUSED: link not encrypted+authenticated");
    sendBleMessage("{\"status\":\"error\",\"message\":\"Not paired\"}");
    return;
  }
  Serial.printf("BLE cmd '%s' accepted (enc=%d auth=%d bond=%d)\n", cmd.c_str(),
                desc.sec_state.encrypted, desc.sec_state.authenticated, desc.sec_state.bonded);
```

`bleConnHandle` is a new file-scope `volatile uint16_t`, set in `onConnect` from
`desc->conn_handle` and reset to `BLE_HS_CONN_HANDLE_NONE` in `onDisconnect`. `handleBleCommand`
runs on the loop task, so reading the handle there and calling `ble_gap_conn_find` is the
hand-off-via-flag pattern the threading rules require — no BLE-task work moves into loop() and no
loop() work moves into a callback.

The `"Not paired"` message string matters: the app already keys `needsPairing` off a refusal it
recognises, and this is what the M5Dial will now send where it used to send `"Not in Config Mode"`.

---

## Move 3 — The atomic write

`save_profiles` currently does `LittleFS.open("/profiles.json", "w")` (line 1029) — it truncates
the only good copy before writing a byte, and never checks the serialize result. A power loss or
a full filesystem leaves an unparseable config recoverable only by reflash. Port the Waveshare's
Serial lines with it — `save_profiles: committed <N> bytes` on success and the
`expected=/written=/onDisk=` line on failure — since verification criterion 6 reads them.

Port the Waveshare's write-temp-then-rename: remove any stale `/profiles.json.tmp`, write to it,
compare `measureJson` against `serializeJson`'s return **and** against `verify.size()` from a
re-opened handle, then `rename` — with the remove-then-retry fallback for LittleFS builds that
refuse to rename onto an existing file. On any failure before the rename, the original is
untouched: report the error and do **not** reload.

**One deliberate divergence.** The Waveshare defers the reload (setting `profilesDirty`) because
its renderer reads `profilesDoc` from the LVGL task. The M5Dial has no second task touching that
document — it draws from `loop()` — so it keeps its existing immediate sequence:

```c
      killAllMacros();
      loadProfiles();
```

`killAllMacros()` before `loadProfiles()` is not optional and not incidental: the macro engine
holds `JsonObject` references into `profilesDoc`, which `deserializeJson` invalidates. This is the
threading rule that has already caused shipped bugs; it survives the port unchanged.

---

## Move 4 — The passkey screen

`onPassKeyNotify` runs on the BLE host task and must not draw. It sets two `volatile` globals and
returns. `loop()` paints.

The M5Dial paints imperatively (direct M5GFX calls) rather than declaratively like LVGL, so it
needs explicit **edge detection** — paint on the false→true transition, `requestRedraw()` on
true→false. Without it, either the screen repaints at loop rate or it never clears. Add to `loop()`
before the mode dispatch:

```c
  static bool wasPairing = false;
  bool isPairing = pairingActive;
  if (isPairing != wasPairing) {
    if (isPairing) drawPairingScreen();
    else           requestRedraw();   // restore whatever mode was underneath
    wasPairing = isPairing;
  }
```

`drawPairingScreen()` mirrors `enterConfigMode()`'s style:

```c
void drawPairingScreen() {
  auto& d = M5Dial.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setFont(&fonts::Orbitron_Light_24);
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.drawString("PAIRING", 120, 60);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Enter PIN:", 120, 110);
  char buf[8];
  snprintf(buf, sizeof(buf), "%06u", (unsigned)currentPasskey);
  d.drawString(buf, 120, 150);
}
```

**Input suppression.** While `pairingActive`, `loop()` skips the `RUN_MODE` encoder, `BtnA`, touch,
and NeoTrellis handling — early-return the input section rather than the whole loop, so the BLE
queue drain and the connect/disconnect transitions still run. Someone reading a PIN off the glass
should not be able to fire a macro into their host by brushing it. This mirrors the Waveshare's
full-screen overlay, which blocks input by covering it.

The passkey screen must also not be clobbered by a queued `uiNeedsRedraw` (line 1484). Gate that
dispatch on `!pairingActive`.

---

## Move 5 — App copy follow-through

Enabling the gate makes copy in the companion app actively wrong — including copy added last week.
Shipping the firmware without this tells users to do the wrong thing.

**`draupnir_state.dart:455`** — `pairingRequiredMessage` names `"Draupnir"` specifically. Both
boards now require pairing, and the message is shown without knowing which board refused:

```dart
  static const String pairingRequiredMessage =
      'This Draupnir is not paired with your phone yet.\n\n'
      'Open your phone\'s Bluetooth settings, pair with it ("Draupnir" for the Waveshare knob, '
      '"Draupnir_Mini" for the M5Dial), and enter the PIN shown on the device\'s screen. '
      'Then come back and connect again.';
```

**`dashboard_screen.dart:571-578`** — the first-run connection bar says
`M5Dial ("Draupnir_Mini"): no pairing`, which becomes false. Replace with one instruction covering
both boards, and update the comment above it, which asserts the same wrong fact.

**Keep `needsConfigMode` and the CONFIG MODE REQUIRED panel.** It stops being a live path and
becomes the fallback for an M5Dial still running old firmware — the graceful way for it to die.
Add a comment saying exactly that, so the next reader does not delete it as dead code or, worse,
"restore" the firmware gate to match it.

---

## Verification

Positive-test only, on hardware, by the owner. Compile gate first (`arduino-cli compile --fqbn
m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB`).

1. **Wi-Fi is gone.** Board boots, ring renders, macros fire. `draupnir.local` does not resolve and
   port 80 refuses. **And BLE re-advertises after the app disconnects** — reconnect without
   rebooting the board. *(This is the Move 1 deletion trap; it is a criterion, not a footnote.)*
2. **Pairing works.** The phone prompts to pair, a 6-digit PIN appears on the dial, entering it
   completes. Serial shows `encrypted=1 authenticated=1 bonded=1`.
3. **The gate does something.** With the dial in **Run Mode** — not Config Mode — the app reads
   profiles and saves an edit successfully. This is what proves the Config Mode check is gone and
   the encrypted link is what authorizes.
4. **Reconnect is silent.** Disconnect and reconnect: no second PIN prompt. The bond persisted.
5. **The backstop agrees.** Serial shows `BLE cmd '<name>' accepted (enc=1 auth=1 bond=1)` for
   every command. If it ever prints `enc=0` or `auth=0` and the command still ran, the permission
   flags are not enforcing and the build guards missed it.
6. **Atomic write.** A save succeeds and Serial shows `save_profiles: committed <N> bytes` with no
   `write failed` or `rename into place failed` line. The edit survives a power cycle. (The save
   path removes any stale `/profiles.json.tmp` on entry, so a second successful save also confirms
   nothing was left behind.)
7. **Regression.** Existing profile intact after the flash; macros fire and stop; swipe-up kill-all
   clears the NeoTrellis LEDs; swipe-down still opens the (now informational) Config Mode screen.

### What this round does not prove

**The negative test is not being run.** Nobody will attempt an unbonded write with nRF Connect
this round. So criterion 5 shows the gate accepting authorized traffic; it does not demonstrate
the gate *refusing* hostile traffic. The compensating controls are the build guards (Move 2),
which make a silently-zero permission flag a compile error, and the runtime `sec_state` check,
which refuses an unauthenticated command even if the flags fail.

This must be recorded as an open item in `docs/HANDOFF.md` §4 in those words. The Waveshare's
equivalent claim ("the H1 negative test PASSED on hardware") is backed by an actual hostile-central
session; the M5Dial's will not be, and the docs must not blur the two. Running nRF Connect against
the M5Dial later is cheap and closes it.

### Breaking change

Every existing M5Dial user must pair the board in phone Bluetooth settings before the app works
again. There is no migration — that is inherent to moving from no security to bonding, and Move 5
is what makes the app say so instead of showing a bare connection failure.

## Task order

Moves 1 → 2 → 3 → 4 → 5, one commit each. Move 1 must land first: doing the BLE work while the
web API is still open would produce a build that looks hardened and is not. Moves 3, 4 and 5 are
independent of each other and could reorder, but 4 (the passkey screen) has no purpose until 2 is
in, and 5 documents behaviour 2 creates.
