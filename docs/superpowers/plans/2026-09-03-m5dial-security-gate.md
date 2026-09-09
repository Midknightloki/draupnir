# M5Dial Security Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the M5Dial's config channel — delete the LAN-reachable web API, enforce BLE pairing/bonding with GATT permission flags, and make profile writes atomic.

**Architecture:** Five sequential commits against one Arduino sketch plus one Flutter app. Move 1 deletes the Wi-Fi/HTTP surface entirely. Move 2 ports the Waveshare knob's proven `BLESecurity` block, GATT permission flags, and adds a runtime `sec_state` backstop. Move 3 ports the atomic write. Move 4 adds the passkey display. Move 5 fixes app copy that Move 2 makes false. Task 6 is the human hardware round.

**Tech Stack:** Arduino ESP32 (m5stack:esp32 3.3.8, NimBLE-backed), M5Unified/M5GFX, LittleFS, ArduinoJson 7.4.3, TinyUSB HID; Flutter + flutter_blue_plus.

**Spec:** `docs/superpowers/specs/2026-09-03-m5dial-security-gate-design.md` — read it first. It carries the header-verification evidence and the reasoning behind every choice below.

## Global Constraints

**Read these before writing a line. Several are load-bearing and have caused shipped bugs.**

1. **No host test framework, no CI in this repo.** There is no `pytest`/`npm test` to run. The automated gate for every firmware task is **`arduino-cli compile` succeeding**; correctness is confirmed by a human on hardware in Task 6. Do **not** invent a test framework, add one, or write tests that assert nothing to satisfy a TDD shape.
2. **No Wi-Fi, no on-device HTTP server, no captive portal, no Wi-Fi provisioning, no web UI.** Permanently cut, not deferred (`CLAUDE.md`, spec v3 §1/§7). If a fix seems to need one, it is the wrong fix.
3. **No application-layer auth token.** The `pairingToken` / `pair` / `token` scheme was removed in M6 and must not be reintroduced in any form, including as a fallback. The rebuttal is in the header comment of `firmware/M5_M6_config/M5_M6_config.ino` (lines 34-59) and `docs/HANDOFF.md` §8.
4. **Verify BLE APIs against the installed core's headers**, at `C:\Users\ido11\AppData\Local\Arduino15\packages\m5stack\hardware\esp32\3.3.8\libraries\BLE\src\`. This core is NimBLE-backed despite Bluedroid-styled class names; the wrong call compiles cleanly and enforces nothing. `BLECharacteristic::setAccessPermissions()` is a no-op here — **never use it**.
5. **Threading rules.** The macro engine is `loop()`-task only. BLE callbacks run on the BLE host task and must hand off via flags/queues drained in `loop()` — **never draw to the display from a BLE callback**. Stop all running macros before reloading profiles (`killAllMacros()` before `loadProfiles()`), because the engine holds `JsonObject` refs into `profilesDoc` that `deserializeJson` invalidates.
6. **Advertised name is exactly `Draupnir_Mini`** (M5Dial) and `Draupnir` (Waveshare). Do not change either.
7. **Do not port `updateConnParams()`** from the Waveshare. It is a board-specific USB HID starvation fix and would confound this milestone's verification.
8. **Do not claim hardware verification that was not performed.** In particular, the negative (hostile-central) test is **not** run this round; Task 6 records that explicitly.
9. Comments explaining *why* are part of the deliverable here, not decoration. This file's existing comments are how the project's hard-won findings survive; match their density and tone.

**Compile gate command** (used by every firmware task):

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB firmware/M5_M6_config
```

---

## File Structure

| File | Change | Responsibility after this plan |
|---|---|---|
| `firmware/M5_M6_config/M5_M6_config.ino` | Modify (Tasks 1-4) | The whole M5Dial firmware. Monolithic by inheritance; this plan does **not** refactor it — that is M5Dial catch-up work with its own milestone. |
| `firmware/M5_M6_config/index_html.h` | **Delete** (Task 1) | Gone. It was the web UI's page body. |
| `companion_app/lib/state/draupnir_state.dart` | Modify (Task 5) | BLE transport + connection state. One copy constant changes. |
| `companion_app/lib/screens/dashboard_screen.dart` | Modify (Task 5) | Connection UI. One copy block and its comment change. |
| `docs/HANDOFF.md` | Modify (Task 6) | Records what was and was not verified. |
| `docs/Draupnir_Spec.md` | Modify (Task 6) | §7 Security stops saying "Waveshare only". |

Deliberately **not** split into `.cpp`/`.h` modules the way `Waveshare_LVGL_Test/` is. That factoring is the target the M5Dial should converge on (`CLAUDE.md`), but doing it inside a security change would bury the security diff in a 1600-line move and make the review worthless.

---

### Task 1: Delete Wi-Fi and the web server

**Files:**
- Modify: `firmware/M5_M6_config/M5_M6_config.ino`
- Delete: `firmware/M5_M6_config/index_html.h`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: an `AppMode` enum with exactly two values, `enum AppMode { RUN_MODE, CONFIG_MODE };`. Later tasks rely on `WIFI_SETUP_MODE` being gone. Also produces a `loop()` whose disconnect branch still calls `BLEDevice::startAdvertising()`.

**Why first:** doing the BLE work while `POST /api/trigger` is still open would produce a build that looks hardened and is not.

- [ ] **Step 1: Delete the five includes**

In `M5_M6_config.ino` lines 5-8 and 13, delete these five lines:

```c
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <WebServer.h>
```

and

```c
#include "index_html.h"
```

Leave every other include alone.

- [ ] **Step 2: Delete the header file**

```bash
git rm firmware/M5_M6_config/index_html.h
```

- [ ] **Step 3: Shrink the AppMode enum**

Line 32. Replace:

```c
enum AppMode { RUN_MODE, CONFIG_MODE, WIFI_SETUP_MODE };
```

with:

```c
// WIFI_SETUP_MODE is gone with the rest of the Wi-Fi surface (spec v3 §1 cuts the web config
// path permanently). CONFIG_MODE survives as an informational screen, not a gate -- see the
// note above handleBleCommand.
enum AppMode { RUN_MODE, CONFIG_MODE };
```

- [ ] **Step 4: Delete the two Wi-Fi globals**

Line 101, delete:

```c
WebServer server(80);
```

Line 107, delete:

```c
bool mdnsStarted = false;
```

- [ ] **Step 5: Delete isAuthorized() and setupWebServer()**

Delete the whole run from the comment above `isAuthorized()` (which begins `// for removal (spec v3 §1 cuts the web config path permanently)`, around line 1185) through the closing brace of `setupWebServer()` and its trailing `server.begin();` — lines ~1180-1291. That block contains `isAuthorized()`, the `/` route, both `/api/profiles` routes, the `/api/trigger` route, and `server.begin()`. Nothing in it survives.

Verify nothing else references them:

```bash
grep -n "isAuthorized\|setupWebServer\|INDEX_HTML" firmware/M5_M6_config/M5_M6_config.ino
```

Expected: no output.

- [ ] **Step 6: Replace the IP address on the Config Mode screen**

In `enterConfigMode()`, replace these three lines:

```c
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("IP Address:", 120, 110);
  d.drawString(WiFi.localIP().toString(), 120, 140);
```

with:

```c
  // Was the Wi-Fi IP address, back when this screen told you where to point a browser. BLE is
  // the only transport now, so the useful facts are which board this is and whether the app is
  // actually attached.
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("BLE", 120, 100);
  d.drawString("Draupnir_Mini", 120, 130);
  d.setTextColor(deviceConnected ? TFT_GREEN : TFT_DARKGRAY, TFT_BLACK);
  d.drawString(deviceConnected ? "app connected" : "waiting for app", 120, 160);
```

- [ ] **Step 7: Delete enterWiFiSetupMode()**

Delete the entire function, lines ~1314-1341 — from `void enterWiFiSetupMode() {` through its closing `}` after `ESP.restart();`.

- [ ] **Step 8: Delete the Wi-Fi bring-up in setup()**

Lines ~1382-1385, delete:

```c
  WiFi.mode(WIFI_STA);
  WiFi.begin();

  setupWebServer();
```

- [ ] **Step 9: Delete the mDNS block and handleClient from the RUN_MODE branch**

In `loop()`, delete `server.handleClient();` (line ~1450) and the mDNS block that follows it:

```c
    if (WiFi.status() == WL_CONNECTED && !mdnsStarted) {
      if (MDNS.begin("draupnir")) {
        Serial.println("MDNS started: draupnir.local");
        MDNS.addService("http", "tcp", 80);
      }
      Serial.print("Wi-Fi connected! IP: ");
      Serial.println(WiFi.localIP());
      mdnsStarted = true;
    }
```

so the branch now opens directly with `updateMacros();`.

- [ ] **Step 10: Delete the BtnA-hold Wi-Fi setup trigger**

Lines ~1512-1516, delete:

```c
    if (M5Dial.BtnA.wasHold()) {
      M5Dial.Speaker.tone(2000, 100);
      waitRelease = true;
      enterWiFiSetupMode();
    }
```

- [ ] **Step 11: Delete the CONFIG_MODE handleClient and the whole WIFI_SETUP_MODE branch**

In the `CONFIG_MODE` branch, delete the single line `server.handleClient();`.

Then delete the entire trailing branch:

```c
  } else if (currentMode == WIFI_SETUP_MODE) {
    server.handleClient();
    
    if (waitRelease && M5Dial.BtnA.isReleased()) {
      waitRelease = false;
    }
    
    if (!waitRelease && M5Dial.BtnA.wasPressed()) {
      ESP.restart();
    }
  }
```

leaving the `CONFIG_MODE` branch's closing `}` to end the chain.

- [ ] **Step 12: Delete the now-unused waitRelease global**

Line 1440, delete:

```c
bool waitRelease = false;
```

Confirm it is unreferenced:

```bash
grep -n "waitRelease" firmware/M5_M6_config/M5_M6_config.ino
```

Expected: no output.

- [ ] **Step 13: Strip Wi-Fi from the connect/disconnect transitions — THE TRAP**

This is the step that breaks BLE if done carelessly. The Wi-Fi pause/restore blocks *contain* the BLE re-advertise logic. Replace the whole pair (lines ~1605-1626) with:

```c
  // BLE reconnection handling
  if (!deviceConnected && oldDeviceConnected) {
    delay(500); // give the bluetooth stack the chance to get ready
    BLEDevice::startAdvertising(); // restart advertising (pServer->startAdvertising silently fails on ESP32)
    Serial.println("Restart BLE advertising");
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    // Was: server.stop(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF). WiFi/BLE radio coexistence
    // was the leading suspect for large BLE notify chunks being silently dropped (verified: ~20B
    // chunks were reliable, 500B chunks weren't), so WiFi was paused for the duration of every
    // BLE session. Deleting WiFi outright subsumes that fix -- contention cannot recur on a radio
    // that is never brought up. Kept as an explicit branch because the transition pair is a
    // recognisable idiom and the disconnect side above still does real work.
    oldDeviceConnected = deviceConnected;
  }
```

**Do not** drop `BLEDevice::startAdvertising()` or either `oldDeviceConnected = deviceConnected;`. Losing the first means the board never re-advertises after a disconnect and looks like a dead radio.

- [ ] **Step 14: Confirm every Wi-Fi symbol is gone**

```bash
grep -n "WiFi\|WebServer\|MDNS\|ESPmDNS\|server\.\|WIFI_SETUP\|handleClient\|INDEX_HTML" firmware/M5_M6_config/M5_M6_config.ino
```

Expected: only comment lines that mention WiFi historically (the two explanatory comments added in Steps 6 and 13, and the pre-existing chunking comment near line 701). **No code.** If any executable line matches, it was missed.

- [ ] **Step 15: Compile**

Run:

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB firmware/M5_M6_config
```

Expected: PASS. A failure naming `WiFi`, `server`, `MDNS`, `waitRelease`, or `WIFI_SETUP_MODE` means a reference was missed — fix it rather than restoring the include.

- [ ] **Step 16: Commit**

```bash
git add firmware/M5_M6_config/
git commit -m "feat: delete the M5Dial web server and all Wi-Fi

POST /api/profiles rewrote the macro set and POST /api/trigger fired
macros -- i.e. injected keystrokes into the attached host -- gated on
nothing but 'are we in Config Mode', reachable by anything on the LAN
with no proximity requirement and no user-visible signal. mDNS even
advertised it as draupnir.local.

Wi-Fi is cut permanently, not deferred (spec v3 s1/s7). The BLE
re-advertise on disconnect lived inside the Wi-Fi restore block and is
deliberately preserved."
```

---

### Task 2: BLE pairing, GATT permission flags, and the runtime backstop

**Files:**
- Modify: `firmware/M5_M6_config/M5_M6_config.ino`

**Interfaces:**
- Consumes: the two-value `AppMode` enum from Task 1.
- Produces, at file scope, for Tasks 3 and 4:
  - `static volatile bool pairingActive;` — true between `onPassKeyNotify` and `onAuthenticationComplete`
  - `static volatile uint32_t currentPasskey;` — the 6-digit code to display
  - `static volatile uint16_t bleConnHandle;` — `BLE_HS_CONN_HANDLE_NONE` when disconnected
  - `void drawPairingScreen();` — **declared** here as a forward declaration, **defined** in Task 4

**Reference implementation:** `firmware/Waveshare_LVGL_Test/ble_engine.cpp` lines 526-700. It is proven on hardware; port faithfully except where noted.

- [ ] **Step 1: Delete the dead BLE2902 include and descriptor**

Line 19, delete `#include <BLE2902.h>`.

Line ~1417, delete:

```c
  pTxCharacteristic->addDescriptor(new BLE2902());
```

NimBLE creates the CCCD itself, and `BLECharacteristic::addDescriptor()` early-returns under `#ifdef CONFIG_NIMBLE_ENABLED` on this core — the call was already dead code.

- [ ] **Step 2: Add the build-time guards**

At **file scope**, immediately after the `#include <BLEUtils.h>` line (they need the BLE headers to have defined the symbols, and namespace scope is where a `static_assert` belongs):

```c
// These two guards exist because this API's signature failure mode is compiling cleanly and
// enforcing nothing. BLECharacteristic.h:162-194 defines PROPERTY_WRITE_ENC / _AUTHEN as
// literally 0 under #if defined(CONFIG_BLUEDROID_ENABLED), and as the real BLE_GATT_CHR_F_*
// bits only under #if defined(CONFIG_NIMBLE_ENABLED). Build against the wrong one and the
// permission flags below become a no-op that no test short of a hostile central would catch.
#if !defined(CONFIG_NIMBLE_ENABLED) && !defined(CONFIG_BT_NIMBLE_ENABLED)
#error "Expected a NimBLE-backed core. Under Bluedroid the GATT permission flags in setup() are 0 and enforce nothing."
#endif
static_assert(BLECharacteristic::PROPERTY_WRITE_ENC != 0,
              "PROPERTY_WRITE_ENC is 0 -- the RX permission flags would be a silent no-op.");
static_assert(BLECharacteristic::PROPERTY_WRITE_AUTHEN != 0,
              "PROPERTY_WRITE_AUTHEN is 0 -- the RX permission flags would be a silent no-op.");
```

- [ ] **Step 3: Add the three shared globals and the forward declaration**

At file scope, next to the other BLE globals near line 63-64 (`BLECharacteristic *pTxCharacteristic` / `bool deviceConnected`):

```c
// Set on the BLE host task, read from loop(). volatile, and never touched by anything that
// draws -- the display is loop()-only (see the pairing screen dispatch in loop()).
static volatile bool pairingActive = false;
static volatile uint32_t currentPasskey = 0;
// BLE_HS_CONN_HANDLE_NONE (0xffff, host/ble_hs.h) when nothing is connected.
static volatile uint16_t bleConnHandle = BLE_HS_CONN_HANDLE_NONE;

void drawPairingScreen(); // defined further down, near enterConfigMode()
```

- [ ] **Step 4: Add SecurityCallbacks**

Immediately above `class MyServerCallbacks` (line ~1080):

```c
// IO_CAP_OUT: this device can only DISPLAY a passkey, not accept input -- the phone/app side
// enters what we show here. Regenerated per-connection (regenPassKeyOnConnect) so it's a fresh
// random code each pairing, not a fixed shared secret.
class SecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; } // we never have input capability
  void onPassKeyNotify(uint32_t pass_key) override {
    // Runs on the BLE host task. Sets flags ONLY -- drawing to M5Dial.Display from here would
    // race the loop() task that owns the panel. loop() watches pairingActive and paints.
    currentPasskey = pass_key;
    pairingActive = true;
    Serial.printf("[ble] show passkey: %06u\n", (unsigned)pass_key);
  }
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t pin) override { return true; }
  // This core (m5stack:esp32 3.3.8) is NimBLE-backed despite the Bluedroid-styled class names,
  // confirmed in its sdkconfig.h. BLESecurity.h declares BOTH overloads -- esp_ble_auth_cmpl_t
  // (line 228) and ble_gap_conn_desc* (line 238). Override the latter; the former is never
  // called here and overriding it would look correct and never fire.
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    pairingActive = false;
    Serial.printf("[ble] authentication complete, encrypted=%d authenticated=%d bonded=%d\n",
                  desc->sec_state.encrypted, desc->sec_state.authenticated, desc->sec_state.bonded);
  }
};
```

- [ ] **Step 5: Switch MyServerCallbacks to the NimBLE overloads**

Replace the existing `onConnect(BLEServer* pServer)` and `onDisconnect(BLEServer* pServer)` with the two-argument forms (`BLEServer.h:306-307`), keeping the existing Serial diagnostics and the existing `bleRxLen = 0;`:

```c
class MyServerCallbacks: public BLEServerCallbacks {
    // NimBLE overload -- gives us the connection handle, so we can request security immediately
    // and so handleBleCommand() can look the link's security state up later.
    void onConnect(BLEServer* pServer, ble_gap_conn_desc* desc) override {
      deviceConnected = true;
      bleConnHandle = desc->conn_handle;
      int rc = 0;
      bool started = BLESecurity::startSecurity(desc->conn_handle, &rc);
      // startSecurity is only a REQUEST, which a hostile central is free to ignore. The GATT
      // permission flags on RX/TX are what actually enforce; this just gets a well-behaved
      // client prompted promptly instead of on its first rejected write.
      Serial.printf("BLE Client Connected, conn_handle=%d startSecurity ok=%d rc=%d connectedCount=%d\n",
                    desc->conn_handle, started, rc, pServer->getConnectedCount());
    };

    void onDisconnect(BLEServer* pServer, ble_gap_conn_desc* desc) override {
      deviceConnected = false;
      pairingActive = false;
      bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
      // A half-received command from a dropped connection must not poison the next one.
      bleRxLen = 0;
      Serial.print("BLE Client Disconnected, connectedCount=");
      Serial.println(pServer->getConnectedCount());
    };
};
```

If the existing `onDisconnect` body has additional Serial lines beyond these, keep them.

**Do not** add `pServer->updateConnParams(...)` — see Global Constraint 7.

- [ ] **Step 6: Add the BLESecurity block in setup()**

In `setup()`, immediately after `BLEDevice::setMTU(512);` and **before** `pServer = BLEDevice::createServer();`:

```c
  BLESecurity::setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  BLESecurity::setCapability(ESP_IO_CAP_OUT);
  BLESecurity::setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setPassKey(false, 0); // false = randomly generated, not this fixed value
  // A passkey fixed for the board's uptime is a weaker secret than a per-pairing one, and with
  // bonding working the user only ever types it once anyway.
  //
  // ROLLBACK LADDER, in order, if unexplained resets into the ROM bootloader appear (this
  // happened on the Waveshare during M6/H1 -- see docs/M6_Hardening_WorkOrder.md):
  //   1. flip this to false,
  //   2. drop PROPERTY_WRITE_AUTHEN from the RX characteristic below,
  //   3. drop the ENC flags entirely -- which is this board's pre-M6 behaviour and is INSECURE.
  //      Do NOT stop at step 3 and call it done; report the boot reason instead.
  BLESecurity::regenPassKeyOnConnect(true);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());
```

- [ ] **Step 7: Put the enforcement bits on RX and TX**

Replace the two `createCharacteristic` calls (lines ~1413-1424) with:

```c
  // ---------------------------------------------------------------------------------------
  // GATT permission enforcement. This is what the stack actually enforces; the
  // BLESecurity::startSecurity() call in MyServerCallbacks::onConnect() is only a request a
  // hostile central can ignore. Without these bits any central in radio range could write
  // save_profiles or trigger -- i.e. inject keystrokes into the attached host.
  //
  // READ THIS BEFORE CHANGING IT. The permission API here differs from every Bluedroid example
  // online. BLECharacteristic::setAccessPermissions() is a NO-OP on this core: its body is
  // wrapped in #ifdef CONFIG_BLUEDROID_ENABLED, and BLEService::start() builds
  // ble_gatt_chr_def.flags from m_properties, never from m_permissions. The enforcement bits
  // live in the PROPERTIES bitmask instead. ENC = "encrypted link"; AUTHEN additionally means
  // the key came from an MITM-protected pairing (our passkey display).
  //
  // KNOWN GAP: BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN (0x10000) cannot be applied through this
  // wrapper -- BLECharacteristic.h:245 stores properties in esp_gatt_char_prop_t, a uint16_t,
  // so the bit is silently truncated. The CCCD is therefore gated on encryption but not
  // explicitly on authentication. That should not be exploitable here, because
  // setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND) means this device will not complete a
  // Just Works pairing at all, so any encrypted link is necessarily an authenticated one.
  //
  // On the Waveshare that last sentence was upgraded from inference to observation by a hostile
  // -central hardware test (nRF Connect, 2026-08-07). On THIS board it remains an inference --
  // the negative test has not been run here. Do not copy the Waveshare's "VERIFIED" wording
  // across until someone actually runs it.
  //
  // It reopens the moment setAuthenticationMode() is relaxed away from *_MITM_*.
  pTxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC
                      );
  pTxCharacteristic->setCallbacks(new TxLogCallbacks());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                                           CHARACTERISTIC_UUID_RX,
                                           BLECharacteristic::PROPERTY_WRITE |
                                           BLECharacteristic::PROPERTY_WRITE_NR |
                                           BLECharacteristic::PROPERTY_WRITE_ENC |
                                           BLECharacteristic::PROPERTY_WRITE_AUTHEN
                                         );
  pRxCharacteristic->setCallbacks(new MyCallbacks());
```

- [ ] **Step 8: Replace the Config Mode gate with the security backstop**

In `handleBleCommand`, replace lines ~931-936:

```c
  // The `pair` command and the per-request `token` check are removed (spec v3 §7, M6/H1) --
  // see the note at the top of this file. Config access is gated on CONFIG_MODE.
  if (currentMode != CONFIG_MODE) {
    sendBleMessage("{\"status\":\"error\",\"message\":\"Not in Config Mode\"}");
    return;
  }
```

with:

```c
  // The `pair` command and the per-request `token` check are removed (spec v3 §7, M6/H1) -- see
  // the note at the top of this file. The CONFIG_MODE check that stood here is removed too: it
  // was a physical-presence gate, not authentication, and it stopped nothing once the user
  // swiped down. An encrypted, MITM-authenticated link is the authorization now. CONFIG_MODE
  // survives as an informational screen.
  //
  // Belt-and-braces behind the GATT permission flags on RX (see setup()). If those ever fail to
  // enforce -- the documented failure mode of this API -- this refuses the command anyway and
  // says so on Serial, rather than executing it quietly. Runs on the loop task, reading a handle
  // the BLE task published: the hand-off-via-flag pattern the threading rules require.
  ble_gap_conn_desc desc;
  if (bleConnHandle == BLE_HS_CONN_HANDLE_NONE ||
      ble_gap_conn_find(bleConnHandle, &desc) != 0 ||
      !desc.sec_state.encrypted || !desc.sec_state.authenticated) {
    Serial.println("[ble] cmd REFUSED: link not encrypted+authenticated");
    sendBleMessage("{\"status\":\"error\",\"message\":\"Not paired\"}");
    return;
  }
  Serial.printf("[ble] cmd '%s' accepted (enc=%d auth=%d bond=%d)\n", cmd.c_str(),
                desc.sec_state.encrypted, desc.sec_state.authenticated, desc.sec_state.bonded);
```

The `"Not paired"` message string is load-bearing: Task 5's app keys its pairing-required UI off a refusal it recognises, and this is what the M5Dial now sends where it used to send `"Not in Config Mode"`.

- [ ] **Step 9: Compile**

Run the compile gate command. Expected: PASS.

If it fails with `'ble_gap_conn_find' was not declared`, add `#include <host/ble_gap.h>` after the BLE includes — the spec anticipated this; it is a loud failure, not a silent one. If either `static_assert` fires, **stop and escalate**: the build is Bluedroid-backed and the permission flags would be no-ops.

- [ ] **Step 10: Commit**

```bash
git add firmware/M5_M6_config/M5_M6_config.ino
git commit -m "feat: enforce BLE pairing on the M5Dial config channel

The M5Dial had no cryptographic access control at all -- no pairing, no
bonding, no GATT permission flags. Any central in radio range could write
save_profiles or trigger. Ports the Waveshare's BLESecurity block,
SecurityCallbacks, and RX/TX permission flags.

The CONFIG_MODE check is deleted: it was a physical-presence gate, not
authentication, and it gated nothing once the user swiped down.

Adds a #error, two static_asserts, and a runtime sec_state check, because
this API's failure mode is compiling cleanly and enforcing nothing --
PROPERTY_WRITE_ENC is literally 0 under Bluedroid."
```

---

### Task 3: Atomic profile write

**Files:**
- Modify: `firmware/M5_M6_config/M5_M6_config.ino` (the `save_profiles` branch of `handleBleCommand`, around line 1029)

**Interfaces:**
- Consumes: nothing new from Tasks 1-2.
- Produces: Serial lines `[ble] save_profiles: committed <N> bytes` on success and `[ble] save_profiles: write failed (expected=<N> written=<N> onDisk=<N>)` on failure. Task 6 criterion 6 reads them.

- [ ] **Step 1: Replace the truncate-and-write**

Find this block (line ~1029) — note it currently truncates the only good copy before writing a byte and never checks the serialize result:

```c
    File f = LittleFS.open("/profiles.json", "w");
    if (f) {
      serializeJson(profilesObj, f);
      f.close();
      
      killAllMacros();
      loadProfiles();
```

Replace from `File f = ...` down to and including `f.close();` with:

```c
    // Write-temp-then-rename. Opening /profiles.json with "w" directly truncated the only good
    // copy before a single byte of the new one was written, and the serializeJson() byte count
    // was never checked -- a power loss, a full filesystem, or a short write left a truncated,
    // unparseable config with no way back short of a reflash. Nothing here touches
    // /profiles.json until the temp file has been written, closed, and verified on disk.
    static const char *PROFILES_PATH = "/profiles.json";
    static const char *PROFILES_TMP_PATH = "/profiles.json.tmp";

    LittleFS.remove(PROFILES_TMP_PATH); // clear any leftover from a previous failed save
    File f = LittleFS.open(PROFILES_TMP_PATH, "w");
    if (!f) {
      sendBleMessage("{\"status\":\"error\",\"message\":\"Failed to open temp file\"}");
      return;
    }
    size_t expected = measureJson(profilesObj);
    size_t written = serializeJson(profilesObj, f);
    f.close();

    // Re-open to confirm close() actually flushed the full document to flash, rather than
    // trusting the writer's own byte count.
    size_t onDisk = 0;
    File verify = LittleFS.open(PROFILES_TMP_PATH, "r");
    if (verify) {
      onDisk = verify.size();
      verify.close();
    }

    if (expected == 0 || written != expected || onDisk != expected) {
      Serial.printf("[ble] save_profiles: write failed (expected=%u written=%u onDisk=%u)\n",
                    (unsigned)expected, (unsigned)written, (unsigned)onDisk);
      LittleFS.remove(PROFILES_TMP_PATH);
      // The original /profiles.json is untouched, so do NOT reload -- the in-memory document
      // still matches what is on flash.
      sendBleMessage("{\"status\":\"error\",\"message\":\"Failed to write file\"}");
      return;
    }

    if (!LittleFS.rename(PROFILES_TMP_PATH, PROFILES_PATH)) {
      // Some LittleFS/VFS builds refuse to rename onto an existing file. Only now, with a
      // verified-good temp file in hand, is it safe to drop the original.
      LittleFS.remove(PROFILES_PATH);
      if (!LittleFS.rename(PROFILES_TMP_PATH, PROFILES_PATH)) {
        Serial.println("[ble] save_profiles: rename into place failed");
        LittleFS.remove(PROFILES_TMP_PATH);
        // The device now has no config file. loadProfiles() below finds none and regenerates
        // defaults -- the correct recovery, though still a failure from the caller's view.
        killAllMacros();
        loadProfiles();
        sendBleMessage("{\"status\":\"error\",\"message\":\"Failed to commit file\"}");
        return;
      }
    }

    Serial.printf("[ble] save_profiles: committed %u bytes\n", (unsigned)onDisk);

    {
```

- [ ] **Step 2: Reattach the reload and the else-branch**

The old code's `if (f) { ... } else { sendBleMessage("...Failed to write file..."); }` structure is gone — every failure now returns early. Immediately after the `{` that ended Step 1, the surviving success path is unchanged:

```c
      // killAllMacros() BEFORE loadProfiles() is not optional. The macro engine holds
      // JsonObject references into profilesDoc, and deserializeJson() clears and reallocates
      // that document's pool -- reloading with a macro mid-sequence is a use-after-free.
      //
      // Unlike the Waveshare (which defers this via a profilesDirty flag), the M5Dial reloads
      // immediately and correctly: nothing but loop() ever touches profilesDoc here, because
      // this board draws from loop() rather than from a separate LVGL task.
      killAllMacros();
      loadProfiles();
      
      int brightness = profilesDoc["settings"]["brightness"] | 160;
      M5Dial.Display.setBrightness(brightness);
      
      int orientation = profilesDoc["settings"]["orientation"] | 0;
      M5Dial.Display.setRotation(orientation);

      requestRedraw();
      sendBleMessage("{\"status\":\"ok\"}");
    }
```

Then **delete** the old trailing else-branch that followed it:

```c
    } else {
      sendBleMessage("{\"status\":\"error\",\"message\":\"Failed to write file\"}");
    }
```

- [ ] **Step 3: Check the braces balance**

```bash
grep -n "save_profiles" firmware/M5_M6_config/M5_M6_config.ino
```

Then read the whole `save_profiles` branch and confirm it opens and closes cleanly — Step 1 replaced an `if (f) {` with a bare `{`, so an extra or missing brace here is the likely mistake. The compile in Step 4 will catch it, but reading it first is faster than decoding the error.

- [ ] **Step 4: Compile**

Run the compile gate command. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/M5_M6_config/M5_M6_config.ino
git commit -m "fix: make M5Dial profile saves atomic

Opening /profiles.json with 'w' truncated the only good copy before a
byte of the new one was written, and serializeJson's return was never
checked. A power loss or full filesystem left an unparseable config
recoverable only by reflash.

Writes to a temp file, verifies the byte count three ways (measureJson,
serializeJson's return, and the re-opened file's size), then renames.
Keeps the M5Dial's immediate killAllMacros/loadProfiles -- unlike the
Waveshare, nothing but loop() touches profilesDoc on this board."
```

---

### Task 4: The passkey screen

**Files:**
- Modify: `firmware/M5_M6_config/M5_M6_config.ino`

**Interfaces:**
- Consumes from Task 2: `pairingActive`, `currentPasskey`, and the `void drawPairingScreen();` forward declaration.
- Produces: `void drawPairingScreen()` — no args, no return.

**Why this shape:** the M5Dial paints imperatively with direct M5GFX calls, unlike the Waveshare's declarative LVGL overlay. So it needs explicit edge detection — paint on the false→true transition, `requestRedraw()` on true→false. Without it the screen either repaints at loop rate or never clears.

- [ ] **Step 1: Define drawPairingScreen()**

Immediately above `void enterConfigMode()` (line ~1294), matching that function's styling conventions:

```c
// Painted from loop() on the pairingActive false->true edge. Never called from a BLE callback:
// SecurityCallbacks::onPassKeyNotify runs on the BLE host task and only sets flags, because the
// display belongs to the loop() task.
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

- [ ] **Step 2: Add the edge detection to loop()**

In `loop()`, immediately after `M5Dial.update();` and **before** the `if (trellisFound)` block:

```c
  // Edge-triggered, not level-triggered: this display is painted imperatively, so repainting
  // every tick would flicker and never repainting would leave the passkey up forever.
  static bool wasPairing = false;
  bool isPairing = pairingActive;
  if (isPairing != wasPairing) {
    if (isPairing) drawPairingScreen();
    else           requestRedraw(); // restore whatever mode's screen was underneath
    wasPairing = isPairing;
  }
```

- [ ] **Step 3: Suppress the NeoTrellis while pairing**

Replace:

```c
  if (trellisFound) {
    trellis.read();
  }
```

with:

```c
  // Not while the passkey is up: trellis.read() dispatches trellisEvent(), which fires macros.
  if (trellisFound && !isPairing) {
    trellis.read();
  }
```

- [ ] **Step 4: Suppress dial input and the queued redraw while pairing**

In the `RUN_MODE` branch, `updateMacros();` stays unconditional — a macro already mid-sequence must still be serviced. Everything after it gets wrapped. Change the opening from:

```c
  if (currentMode == RUN_MODE) {
    updateMacros();
    
    long newPosRaw = M5Dial.Encoder.read();
```

to:

```c
  if (currentMode == RUN_MODE) {
    updateMacros();

    // The pairing screen owns the display and the inputs while it is up. Someone reading a PIN
    // off the glass must not fire a macro into their host by brushing it. Macros already running
    // are still serviced above; the queued redraw is deliberately NOT dispatched in here either,
    // because drawRunUI() would paint straight over the passkey.
    if (!isPairing) {
    long newPosRaw = M5Dial.Encoder.read();
```

Then close that guard at the end of the `RUN_MODE` branch — after the `if (changed) { ... }` block's closing brace and before the `} else if (currentMode == CONFIG_MODE) {`:

```c
      if (changed) {
        prefs.putInt("activeProfile", activeProfileIdx);
        M5Dial.Speaker.tone(3000, 30);
        killAllMacros(); // Kill macros on profile change just to be safe
        requestRedraw();
      }
    }
    } // end !isPairing
  } else if (currentMode == CONFIG_MODE) {
```

Re-indent the guarded block by one level so the file stays readable. This makes the diff large; that is expected and correct.

- [ ] **Step 5: Suppress the Config Mode exit tap while pairing**

Change the `CONFIG_MODE` branch to:

```c
  } else if (currentMode == CONFIG_MODE) {
    auto touch = M5Dial.Touch.getDetail();
    if (!isPairing && (touch.wasReleased() || M5Dial.BtnA.wasReleased())) {
      currentMode = RUN_MODE;
      M5Dial.Speaker.tone(2000, 30);
      requestRedraw();
    }
  }
```

- [ ] **Step 6: Compile**

Run the compile gate command. Expected: PASS. A brace error here almost certainly means the Step 4 guard was not closed in the right place.

- [ ] **Step 7: Commit**

```bash
git add firmware/M5_M6_config/M5_M6_config.ino
git commit -m "feat: show the BLE passkey on the M5Dial screen

onPassKeyNotify runs on the BLE host task and only sets flags; loop()
watches the flag and paints, because the display belongs to the loop
task. Edge-triggered, since this board paints imperatively rather than
through LVGL's declarative overlay.

Dial, touch, and NeoTrellis input are suppressed while the PIN is up so
that reading it cannot fire a macro into the host. Macros already running
are still serviced."
```

---

### Task 5: App copy follow-through

**Files:**
- Modify: `companion_app/lib/state/draupnir_state.dart:455-458`
- Modify: `companion_app/lib/screens/dashboard_screen.dart:571-579`

**Interfaces:**
- Consumes from Task 2: the M5Dial now answers `{"status":"error","message":"Not paired"}` where it used to answer `"Not in Config Mode"`.
- Produces: no API change — copy only.

**Why this is not optional:** Task 2 makes the app's own first-run instructions false. `dashboard_screen.dart:578` currently reads `M5Dial ("Draupnir_Mini"): no pairing`, which after Task 2 tells users to do the wrong thing.

- [ ] **Step 1: Make pairingRequiredMessage board-agnostic**

The message is shown without knowing which board refused, so it must cover both. Replace:

```dart
  static const String pairingRequiredMessage =
      'This Draupnir is not paired with your phone yet.\n\n'
      'Open your phone\'s Bluetooth settings, pair with "Draupnir", and enter the PIN shown on '
      'the knob\'s screen. Then come back and connect again.';
```

with:

```dart
  static const String pairingRequiredMessage =
      'This Draupnir is not paired with your phone yet.\n\n'
      'Open your phone\'s Bluetooth settings, pair with it ("Draupnir" for the Waveshare knob, '
      '"Draupnir_Mini" for the M5Dial), and enter the PIN shown on the device\'s screen. '
      'Then come back and connect again.';
```

Update the comment two lines above it, which says the message points at "the PIN on the knob's screen" — it is both boards' screens now.

- [ ] **Step 2: Mark the Config Mode path as a legacy fallback**

Directly above `configModeRequiredMessage` (line ~460), replace the existing comment with:

```dart
  // LEGACY FALLBACK -- do not delete, and do not "restore" the firmware gate to match it.
  // The M5Dial's CONFIG_MODE check was removed when it gained real BLE pairing; an encrypted,
  // authenticated link is the authorization now. This message can therefore only come from an
  // M5Dial still running pre-gate firmware, which is exactly why it stays: on that board it is
  // still the correct instruction. Note the gesture named here is the OPPOSITE direction from
  // that board's kill-all (swipe up), so naming the wrong one would be worse than saying nothing.
```

- [ ] **Step 3: Fix the first-run connection bar**

In `dashboard_screen.dart`, replace the comment and `Text` widget at lines ~571-579:

```dart
          // Pairing applies to the Waveshare knob ("Draupnir"), which enforces bonding. The
          // M5Dial ("Draupnir_Mini") has no BLE security yet and needs no pairing -- it needs
          // Config Mode instead, which is what the CONFIG MODE REQUIRED panel says when it
          // refuses. Both are named here so the first-run screen matches either board.
          const Text(
            'Waveshare knob ("Draupnir"): pair it in your phone\'s Bluetooth settings first — '
            'it shows the PIN on its screen.\n'
            'M5Dial ("Draupnir_Mini"): no pairing; swipe down on the dial to enter Config Mode.',
```

with:

```dart
          // Both boards enforce BLE bonding now, so this is one instruction rather than two.
          // It used to say the M5Dial needed no pairing; that stopped being true when the
          // M5Dial security gate landed.
          const Text(
            'Pair your Draupnir in your phone\'s Bluetooth settings first — "Draupnir" is the '
            'Waveshare knob, "Draupnir_Mini" is the M5Dial. Each shows a PIN on its own screen '
            'while pairing. Then come back and connect.',
```

Leave the `style:` and `textAlign:` lines that follow untouched.

- [ ] **Step 4: Analyze**

Run:

```bash
cd companion_app && flutter analyze
```

Expected: no new issues. Pre-existing warnings unrelated to these two files are acceptable — note them in the report rather than fixing them.

- [ ] **Step 5: Commit**

```bash
git add companion_app/lib/state/draupnir_state.dart companion_app/lib/screens/dashboard_screen.dart
git commit -m "fix: app copy said the M5Dial needs no pairing

It does now. The first-run connection bar instructed users to skip the
step that is the whole point of the M5Dial security gate, and
pairingRequiredMessage named only the Waveshare by name.

Keeps the CONFIG MODE REQUIRED panel as an explicitly-labelled fallback
for an M5Dial still running pre-gate firmware."
```

---

### Task 6: Hardware verification round

**Files:**
- Modify: `docs/HANDOFF.md`
- Modify: `docs/Draupnir_Spec.md`

**Interfaces:** Consumes everything. Produces the record of what was and was not proven.

**This task requires the human.** The agent flashes and reads Serial; the owner supplies the G0/BOOT press, operates the phone, and reports what the screen shows. **Do not mark any criterion passed on the agent's inference** — see Global Constraint 8.

- [ ] **Step 1: Flash**

Ask the owner to put the M5Dial in download mode (hold BOOT, replug USB — it enumerates on COM5 in download mode, COM7 when running). Then:

```bash
arduino-cli upload -p COM5 --fqbn m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB firmware/M5_M6_config
```

- [ ] **Step 2: Ask the owner to run the seven criteria**

Present this list and collect the owner's observations verbatim. Note that the M5Dial's bond must be **removed from the phone's Bluetooth settings first** — this is a breaking change and an old entry will not carry over.

1. **Wi-Fi is gone.** Board boots, ring renders, macros fire. `draupnir.local` does not resolve; port 80 refuses. **And BLE re-advertises after the app disconnects** — reconnect without rebooting the board. *(This is the Task 1 deletion trap.)*
2. **Pairing works.** The phone prompts to pair, a 6-digit PIN appears on the dial, entering it completes. Serial shows `encrypted=1 authenticated=1 bonded=1`.
3. **The gate does something.** With the dial in **Run Mode** — not Config Mode — the app reads profiles and saves an edit. This is what proves the CONFIG_MODE check is gone and the encrypted link is what authorizes.
4. **Reconnect is silent.** Disconnect and reconnect: no second PIN. The bond persisted.
5. **The backstop agrees.** Serial shows `[ble] cmd '<name>' accepted (enc=1 auth=1 bond=1)` for every command. If it ever prints `enc=0` or `auth=0` **and the command still ran**, the permission flags are not enforcing — stop and escalate.
6. **Atomic write.** A save succeeds, Serial shows `[ble] save_profiles: committed <N> bytes` with no `write failed` or `rename into place failed` line, and the edit survives a power cycle.
7. **Regression.** Existing profile intact after the flash; macros fire and stop; swipe-up kill-all clears the NeoTrellis LEDs; swipe-down still opens the (now informational) Config Mode screen, showing `Draupnir_Mini` and the connection state instead of an IP.

If any criterion fails, stop and report it. Do not proceed to Step 3 with a failure recorded as a pass.

- [ ] **Step 3: Record the result in docs/HANDOFF.md**

Add to §4 a block headed `Verified on hardware — M5Dial security gate, <date>`, with one bullet per criterion that actually passed, in the owner's terms.

Then add this, verbatim, as its own paragraph — it is the honest limit of the round:

```markdown
**Not verified: the negative test.** No hostile-central attempt (an unbonded write via nRF
Connect) was made against the M5Dial this round. Criterion 5 shows the gate accepting authorized
traffic; it does not demonstrate the gate refusing unauthorized traffic. The compensating controls
are the build guards (`#error` plus two `static_asserts`, which make a silently-zero permission
flag a compile error) and the runtime `sec_state` check, which refuses an unauthenticated command
even if the flags fail. The Waveshare's equivalent claim IS backed by a real hostile-central
session (2026-08-07); the M5Dial's is not, and the two must not be blurred. Running nRF Connect
against the M5Dial later is cheap and closes this.
```

- [ ] **Step 4: Update the spec's Security section**

In `docs/Draupnir_Spec.md` §7, the Security section opens with a paragraph beginning **"Enforced on the Waveshare only, as of 2026-08-29."** stating that the M5Dial has no BLESecurity block, no passkey, no permission flags, and nothing to pair with. That is now false. Rewrite it to say both boards enforce standard BLE pairing with a passkey shown on the device screen, and carry across the negative-test caveat from Step 3 in one sentence.

Also update the "Config Mode gate (M5Dial only)" subsection: the gate is gone; Config Mode is an informational screen.

- [ ] **Step 5: Commit**

```bash
git add docs/HANDOFF.md docs/Draupnir_Spec.md
git commit -m "docs: M5Dial security gate verified on hardware

Records the seven positive criteria that passed, and records explicitly
that the negative (hostile-central) test was NOT run this round -- with
the compensating controls that partly stand in for it. The Waveshare's
negative test was real; the M5Dial's has not happened, and the docs must
not blur the two."
```

---

## Self-Review

**1. Spec coverage.** Move 1 → Task 1; Move 2 → Task 2; Move 3 → Task 3; Move 4 → Task 4; Move 5 → Task 5; Verification (7 criteria + the "what this round does not prove" section + the breaking-change note) → Task 6. The spec's build-guard, rollback-ladder, known-gap, and `updateConnParams` exclusion all appear in Task 2. No gaps.

**2. Placeholder scan.** No TBD/TODO. Every code step carries the actual code. Task 6 Step 4 describes a rewrite rather than quoting the replacement text, because the target paragraph's exact current wording depends on edits landing in Tasks 1-5 — the instruction names the file, section, opening words, and the two facts that must change, which is actionable without a verbatim block.

**3. Type consistency.** `pairingActive` / `currentPasskey` / `bleConnHandle` are declared once in Task 2 Step 3 and used with those exact names in Tasks 2 and 4. `drawPairingScreen()` is forward-declared in Task 2 and defined in Task 4 with a matching `void ()` signature. `isPairing` is the loop-local snapshot introduced in Task 4 Step 2 and used in Steps 3-5 of the same task. `BLE_HS_CONN_HANDLE_NONE` is the real macro (`host/ble_hs.h:57`, `0xffff`). The Serial strings in Task 3 match what Task 6 criterion 6 reads; the `[ble] cmd ... accepted` string in Task 2 matches criterion 5; `"Not paired"` in Task 2 matches what Task 5 Step 2 describes.

**One deviation from the skill's default task shape, stated plainly:** this repo has no host test framework and no CI, so there is no failing-test-first cycle to run. Writing `- [ ] Write the failing test` steps here would produce tests that assert nothing against hardware that isn't attached. The automated gate is the compile, and correctness is established by a human on hardware in Task 6. That is the project's actual verification contract (`CLAUDE.md`, "each verified on hardware before advancing"), not a shortcut around one.
