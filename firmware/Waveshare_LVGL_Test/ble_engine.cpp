#include "ble_engine.h"
#include "macro_engine.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// Same framing M5_M6_config.ino uses: a 1-byte sequence number prefixed onto every TX chunk,
// echoed back by the app as a 2-byte [BLE_CHUNK_ACK_MARKER, seq] write so we know it actually
// arrived (notify() itself has no delivery guarantee -- confirmed on M5Dial: the stack reports
// success even when the central's OS never surfaces the packet).
#define BLE_CHUNK_ACK_MARKER 0xFE
static const int BLE_CHUNK_PAYLOAD_SIZE = 100;

// RX reassembly buffer for chunked incoming commands. Must be malloc()'d, not static/global --
// on the M5Dial firmware a static array here silently broke BLE advertising (see
// docs/BLE_Profile_Fetch_Debugging.md). Allocate once in ble_init() after BLEDevice::init().
//
// Sized for a realistic multi-profile save_profiles document, not a single profile: the
// companion app emits an `icon_xbm` field per macro (an 18x18 1bpp bitmap as a 108-char hex
// string, see companion_app/lib/utils/icon_generator.dart), which puts one fully-populated
// 16-macro profile at roughly 3.5 KB before the save_profiles envelope -- i.e. essentially at
// the old 4096 ceiling, with two profiles clearing it outright. 8192 gives ~2 such profiles of
// headroom while costing only 4 KB more than the old value; it is allocated once in ble_init(),
// before the LVGL draw buffers and the profile JsonDocument, when the heap is still
// unfragmented. Free heap is logged either side of the allocation so the cost is visible in the
// boot trace. Raising this further should be justified against that logged number, not guessed
// -- neither board has PSRAM.
#define BLE_RX_BUFFER_SIZE 8192
static char *bleRxBuf = nullptr;
static size_t bleRxLen = 0;
// Set when a message exceeds BLE_RX_BUFFER_SIZE: the partial message is dropped and every
// further byte is discarded up to and including the terminating '\n', so the tail of the
// oversized command is never parsed as a bogus standalone command. Written from the BLE host
// task (RxCallbacks::onWrite) only.
static bool bleRxDiscarding = false;
// Hand-off to loop(): the overflow error response must be sent from ble_update(), not from the
// BLE callback (same rule as every other TX in this file). Set on the BLE host task, cleared on
// the loop() task.
static volatile bool bleRxOverflowPending = false;

static BLEServer *pServer = nullptr;
static BLECharacteristic *pTxCharacteristic = nullptr;
static QueueHandle_t bleRxQueue = nullptr;   // completed command strings (char*, caller frees)
static QueueHandle_t bleAckQueue = nullptr;  // acked chunk sequence numbers

static volatile bool connected = false;
static volatile bool pairingActive = false;
static volatile uint32_t currentPasskey = 0;
static volatile bool profilesDirty = false;

bool ble_is_connected() { return connected; }
bool ble_pairing_active() { return pairingActive; }
uint32_t ble_passkey() { return currentPasskey; }
bool ble_profiles_dirty() { return profilesDirty; }
void ble_clear_profiles_dirty() { profilesDirty = false; }

// See sendNotifyAndWaitAck()'s use below: blocks until the app echoes this chunk's seq back,
// resending (not just waiting longer) on timeout since a missing ack over BLE genuinely means
// the packet needs resending, not that it's merely late.
static bool sendNotifyAndWaitAck(const uint8_t *data, size_t len, uint8_t seq) {
  uint8_t framed[BLE_CHUNK_PAYLOAD_SIZE + 1];
  framed[0] = seq;
  memcpy(framed + 1, data, len);
  const int maxAttempts = 5;
  const uint32_t attemptTimeoutMs = 800;
  for (int attempt = 1; attempt <= maxAttempts; attempt++) {
    pTxCharacteristic->setValue(framed, len + 1);
    pTxCharacteristic->notify();
    uint32_t deadline = millis() + attemptTimeoutMs;
    while (true) {
      int32_t remaining = (int32_t)(deadline - millis());
      if (remaining <= 0) break;
      uint8_t ackedSeq;
      if (xQueueReceive(bleAckQueue, &ackedSeq, pdMS_TO_TICKS(remaining)) != pdTRUE) break;
      if (ackedSeq == seq) return true;
      // stale ack for an earlier chunk/retry -- keep waiting out the remaining budget
    }
    Serial.printf("[ble] chunk ack timed out seq=%d attempt=%d\n", seq, attempt);
  }
  Serial.println("[ble] chunk ack exhausted, aborting message");
  return false;
}

static void bleSendPreamble() {
  uint8_t dummy;
  while (xQueueReceive(bleAckQueue, &dummy, 0) == pdTRUE) {}
  delay(300); // let the central settle right after CCCD-enable before the first notify
}

// Streams a JSON response into the chunked notify+ack pipeline without ever building the whole
// payload as one big String -- see M5_M6_config.ino's BleChunkSink for the heap-fragmentation
// history this avoids (same wire format, ported near-verbatim).
class BleChunkSink : public Print {
public:
  bool failed = false;
  size_t totalSent = 0;

  size_t write(uint8_t c) override {
    if (failed) return 0;
    _buf[_fill++] = c;
    totalSent++;
    if (_fill == BLE_CHUNK_PAYLOAD_SIZE) {
      if (!sendNotifyAndWaitAck(_buf, _fill, _seq)) { failed = true; return 0; }
      _seq++;
      _fill = 0;
    }
    return 1;
  }
  size_t write(const uint8_t *buffer, size_t size) override {
    size_t n = 0;
    while (n < size && write(buffer[n]) == 1) n++;
    return n;
  }
  bool flushRemainder() {
    if (failed) return false;
    if (_fill > 0) {
      if (!sendNotifyAndWaitAck(_buf, _fill, _seq)) { failed = true; return false; }
      _seq++;
      _fill = 0;
    }
    return true;
  }

private:
  uint8_t _buf[BLE_CHUNK_PAYLOAD_SIZE];
  int _fill = 0;
  uint8_t _seq = 0;
};

static void sendBleMessage(const String &msg) {
  bleSendPreamble();
  int len = msg.length();
  int offset = 0;
  uint8_t seq = 0;
  while (offset < len) {
    int chunkSize = min(BLE_CHUNK_PAYLOAD_SIZE, len - offset);
    if (!sendNotifyAndWaitAck((const uint8_t *)(msg.c_str() + offset), chunkSize, seq)) return;
    offset += chunkSize;
    seq++;
  }
  uint8_t nl = '\n';
  sendNotifyAndWaitAck(&nl, 1, seq);
}

// Runs only from ble_update() (loop() task) -- never call directly from a BLE callback.
static void handleBleCommand(char *cmdStr) {
  Serial.print("[ble] cmd: ");
  Serial.println(cmdStr);
  JsonDocument req;
  if (deserializeJson(req, cmdStr)) {
    sendBleMessage("{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
    return;
  }
  String cmd = req["cmd"] | "";

  if (cmd == "get_profiles") {
    bleSendPreamble();
    BleChunkSink sink;
    sink.print("{\"status\":\"ok\",\"profiles\":");
    profiles_serialize(sink);
    sink.print("}\n");
    if (sink.flushRemainder()) {
      Serial.printf("[ble] get_profiles: streamed %u bytes\n", (unsigned)sink.totalSent);
    } else {
      Serial.println("[ble] get_profiles: send aborted (ack retries exhausted)");
    }
  } else if (cmd == "save_profiles") {
    // Matches M5_M6_config.ino's save_profiles handler: write the app's "profiles" object
    // straight to /profiles.json (it's the FULL document -- version/settings/profiles -- not
    // just the array, same nested shape get_profiles sends), then reload it into profilesDoc.
    JsonObject profilesObj = req["profiles"];
    if (profilesObj.isNull()) {
      sendBleMessage("{\"status\":\"error\",\"message\":\"No profiles object provided\"}");
      return;
    }
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
        // profiles_reload() here would find no file and regenerate defaults, which is the
        // correct recovery for a device that now has no config -- but it is still a failure.
        profiles_reload();
        profilesDirty = true;
        sendBleMessage("{\"status\":\"error\",\"message\":\"Failed to commit file\"}");
        return;
      }
    }

    Serial.printf("[ble] save_profiles: committed %u bytes\n", (unsigned)onDisk);
    profiles_reload();
    profilesDirty = true; // .ino's loop() rebuilds the ring UI from this on its next tick
    Serial.println("[ble] save_profiles: written and reloaded");
    sendBleMessage("{\"status\":\"ok\"}");
  } else if (cmd == "trigger") {
    // Waveshare has no profile-switching UI yet (only one profile is ever "active" at a time),
    // so unlike M5Dial's trigger handler this doesn't honor an arbitrary req["profile"] index --
    // it just test-fires a macro position within whatever profile is currently loaded.
    int macroPos = req["macro"] | -1;
    if (macroPos >= 0 && !profiles_find_macro(macroPos).isNull()) {
      macros_request_fire(macroPos);
      sendBleMessage("{\"status\":\"ok\"}");
    } else {
      sendBleMessage("{\"status\":\"error\",\"message\":\"Macro not found\"}");
    }
  } else {
    sendBleMessage("{\"status\":\"error\",\"message\":\"Unknown command\"}");
  }
}

// Reassembles chunked RX writes into a full command string, exactly matching M5_M6_config.ino's
// scheme: a duplicate-write guard (the RX characteristic's onWrite() fires twice for the same
// value on some stacks/timings) and a '\n'-terminated message boundary. bleRxBuf is malloc'd
// once in ble_init(), not a static array (see the comment above BLE_RX_BUFFER_SIZE).
class RxCallbacks : public BLECharacteristicCallbacks {
  String lastRxValue;
  void onWrite(BLECharacteristic *pCharacteristic) override {
    String rxValue = pCharacteristic->getValue();
    if (rxValue.length() == 2 && (uint8_t)rxValue[0] == BLE_CHUNK_ACK_MARKER) {
      uint8_t seq = (uint8_t)rxValue[1];
      xQueueOverwrite(bleAckQueue, &seq);
      return;
    }
    if (rxValue == lastRxValue) return; // duplicate callback for the same write
    lastRxValue = rxValue;

    if (bleRxBuf == nullptr) return;
    for (size_t i = 0; i < rxValue.length(); i++) {
      char c = rxValue[i];

      // Tail of an oversized message: drop everything up to and including its terminator.
      // Without this the remainder would be reassembled and parsed as its own command.
      if (bleRxDiscarding) {
        if (c == '\n') {
          bleRxDiscarding = false;
          Serial.println("[ble] RX overflow: oversized message fully discarded, channel ready");
        }
        continue;
      }

      if (c == '\n') {
        bleRxBuf[bleRxLen] = '\0';
        char *cmdCopy = strdup(bleRxBuf);
        bleRxLen = 0;
        if (cmdCopy != nullptr && bleRxQueue != nullptr) {
          if (xQueueSend(bleRxQueue, &cmdCopy, 0) != pdTRUE) free(cmdCopy);
        }
      } else {
        // The old loop simply stopped consuming at the ceiling, which also swallowed the '\n'
        // that would have reset bleRxLen -- nothing else ever reset it, so every subsequent
        // command was silently dropped until reboot. Reset and enter discard mode instead.
        if (bleRxLen >= BLE_RX_BUFFER_SIZE - 1) {
          Serial.printf("[ble] RX overflow: message exceeded %u bytes, discarding\n",
                        (unsigned)(BLE_RX_BUFFER_SIZE - 1));
          bleRxLen = 0;
          bleRxDiscarding = true;
          bleRxOverflowPending = true; // ble_update() sends the error from the loop() task
          continue;
        }
        bleRxBuf[bleRxLen++] = c;
      }
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  // NimBLE-specific overload (see the onAuthenticationComplete comment below for why) -- gives
  // us the connection handle so we can explicitly request security right away, instead of
  // relying on per-characteristic ENC permission flags to trigger it implicitly.
  void onConnect(BLEServer *server, ble_gap_conn_desc *desc) override {
    connected = true;
    Serial.printf("[ble] connected, conn_handle=%d\n", desc->conn_handle);
    int rc = 0;
    bool started = BLESecurity::startSecurity(desc->conn_handle, &rc);
    Serial.printf("[ble] startSecurity requested, ok=%d rc=%d\n", started, rc);
    // Explicitly request the widened interval (the advertised preference above is only a hint
    // the central can ignore) plus slave latency 4 -- lets this side skip up to 4 connection
    // events when it has nothing to send, cutting background radio/host activity further while
    // idle-connected, which is when we saw HID output silently stop.
    server->updateConnParams(desc->conn_handle, 0x28, 0x50, 4, 400);
  }
  void onDisconnect(BLEServer *server, ble_gap_conn_desc *desc) override {
    connected = false;
    pairingActive = false;
    Serial.println("[ble] disconnected");
    BLEDevice::startAdvertising(); // pServer->startAdvertising() silently fails on ESP32
  }
};

// IO_CAP_OUT: this device can only DISPLAY a passkey, not accept input -- the phone/app side
// enters what we show here. Regenerated per-connection (regenPassKeyOnConnect) so it's a fresh
// random code each pairing, not a fixed shared secret.
class SecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; } // we never have input capability
  void onPassKeyNotify(uint32_t pass_key) override {
    currentPasskey = pass_key;
    pairingActive = true;
    Serial.printf("[ble] show passkey: %06u\n", (unsigned)pass_key);
  }
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t pin) override { return true; }
  // This core (m5stack:esp32 3.3.8) has CONFIG_BT_NIMBLE_ENABLED, not Bluedroid, despite the
  // classic Bluedroid-styled class names (BLEDevice/BLEServer/BLE2902) -- confirmed directly in
  // this build's sdkconfig.h. The NimBLE overload takes a ble_gap_conn_desc*, not
  // esp_ble_auth_cmpl_t.
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    pairingActive = false;
    Serial.printf("[ble] authentication complete, encrypted=%d authenticated=%d bonded=%d\n",
                  desc->sec_state.encrypted, desc->sec_state.authenticated, desc->sec_state.bonded);
  }
};

void ble_init() {
  bleRxQueue = xQueueCreate(4, sizeof(char *));
  bleAckQueue = xQueueCreate(1, sizeof(uint8_t));

  BLEDevice::init("Draupnir");
  uint32_t heapBefore = ESP.getFreeHeap();
  bleRxBuf = (char *)malloc(BLE_RX_BUFFER_SIZE);
  if (bleRxBuf == nullptr) {
    Serial.printf("[ble] FATAL: failed to allocate %u byte RX buffer (free heap=%u, largest block=%u)\n",
                  (unsigned)BLE_RX_BUFFER_SIZE, (unsigned)heapBefore,
                  (unsigned)ESP.getMaxAllocHeap());
  } else {
    Serial.printf("[ble] RX buffer allocated: %u bytes, heap %u -> %u\n",
                  (unsigned)BLE_RX_BUFFER_SIZE, (unsigned)heapBefore, (unsigned)ESP.getFreeHeap());
  }
  BLEDevice::setMTU(512);

  BLESecurity::setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  BLESecurity::setCapability(ESP_IO_CAP_OUT);
  BLESecurity::setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setPassKey(false, 0); // false = randomly generated once here, not this fixed value
  // Diagnostic step: regenerating a fresh passkey on every reconnect (regenPassKeyOnConnect)
  // plus forcing encryption via per-characteristic ENC flags is suspected of destabilizing this
  // board (repeated unexplained resets into the ROM bootloader, and one get_profiles exchange
  // that the firmware logged as fully sent+acked but the app never actually received/parsed) --
  // simplified to a single random passkey generated once at boot, kept for the board's uptime.
  BLESecurity::regenPassKeyOnConnect(false);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Diagnostic step (see regenPassKeyOnConnect comment above): dropped the PROPERTY_READ_ENC /
  // PROPERTY_WRITE_ENC flags for now. Security is instead requested explicitly and once, via
  // BLESecurity::startSecurity() in ServerCallbacks::onConnect(), rather than relying on
  // per-characteristic permission bits to trigger pairing implicitly.
  pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pRxCharacteristic->setCallbacks(new RxCallbacks());

  pService->start();

  pServer->getAdvertising()->addServiceUUID(SERVICE_UUID);
  pServer->getAdvertising()->setScanResponse(true);
  // Widened from the original 0x06/0x12 (7.5-22.5ms): a tight connection interval means the BLE
  // radio/host task interrupts frequently even when idle, and that appears to starve USB HID
  // report delivery enough that keystrokes stop landing at all while a central is connected
  // (confirmed: HID output works fine disconnected, silently fails while connected). This is
  // just the advertised hint the central may ignore -- see the explicit updateConnParams() call
  // in ServerCallbacks::onConnect() below for the actual enforcement.
  pServer->getAdvertising()->setMinPreferred(0x28); // 50ms
  pServer->getAdvertising()->setMaxPreferred(0x50); // 100ms
  BLEDevice::startAdvertising();
  Serial.println("[ble] advertising as \"Draupnir\"");
}

void ble_update() {
  // Report an RX overflow from here rather than from the callback that detected it -- every TX
  // in this file goes out on the loop() task. Reported as soon as the overflow is seen (not
  // when the oversized message finally terminates) so the app fails fast instead of waiting out
  // its 30s response timeout.
  if (bleRxOverflowPending) {
    bleRxOverflowPending = false;
    sendBleMessage("{\"status\":\"error\",\"message\":\"Command too large\"}");
  }

  char *cmdStr;
  if (bleRxQueue != nullptr && xQueueReceive(bleRxQueue, &cmdStr, 0) == pdTRUE) {
    handleBleCommand(cmdStr); // zero-copy parse; cmdStr must outlive the call
    free(cmdStr);
  }
}
