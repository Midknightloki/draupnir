#include "ota_engine.h"
#include "ota_pubkey.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"

#include "macro_engine.h"
#include "ble_engine.h"

// Binary mode dies after this long without a chunk. A dropped transfer must not leave the device
// in a mode where it no longer parses commands -- that would need a power cycle to recover from.
static const uint32_t OTA_IDLE_TIMEOUT_MS = 15000;

static volatile bool          otaActive = false;
static esp_ota_handle_t       otaHandle = 0;
static const esp_partition_t *otaTarget = nullptr;
static uint32_t               otaExpected = 0;
static uint32_t               otaReceived = 0;
static uint32_t               otaLastFeedMs = 0;
static uint8_t                otaExpectedHash[32];
static uint8_t                otaSig[80];
static size_t                 otaSigLen = 0;
static mbedtls_sha256_context otaSha;

// RECLAIM THE ROLLBACK DECISION FROM THE ARDUINO CORE.
//
// This is not optional and it is not defensive -- without it M13's entire second safety net is
// silently absent, and every claim the design makes about rollback is false.
//
// The core's initArduino() (esp32-hal-misc.c, cores/esp32 3.3.11) runs BEFORE setup() and does:
//
//     #ifdef CONFIG_APP_ROLLBACK_ENABLE
//       if (!verifyRollbackLater()) {
//         ... if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
//           if (verifyOta()) { esp_ota_mark_app_valid_cancel_rollback(); }
//
// CONFIG_APP_ROLLBACK_ENABLE IS defined for this target, and both hooks are weak symbols
// defaulting to verifyRollbackLater() == false and verifyOta() == true. So a freshly-flashed
// OTA image marks ITSELF valid before a single line of our code runs -- meaning any image that
// boots far enough to reach initArduino(), which is essentially any image that boots at all,
// sticks permanently and can never be reverted.
//
// Returning true here defers the decision to us. Confirmation then happens only in
// ota_confirm(), in response to the app reconnecting and asking -- which requires the device to
// boot, initialise BLE, advertise, pair and serve a command. That round trip is exactly the
// capability a broken image would have lost, which is the whole point of the net.
//
// Found by whole-branch review, not by inspection of this repo: the violation lives in the core,
// outside the diff, so grepping firmware/ for the confirm call reported "nothing self-confirms"
// and was wrong.
extern "C" bool verifyRollbackLater() { return true; }

bool ota_active() { return otaActive; }
uint32_t ota_offset() { return otaReceived; }
uint32_t ota_expected() { return otaExpected; }

static int hexbytes(const char *hex, uint8_t *out, size_t outlen) {
  size_t n = strlen(hex);
  if (n % 2 || n / 2 > outlen) return -1;
  for (size_t i = 0; i < n / 2; i++) {
    char pair[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
    char *end = nullptr;
    long v = strtol(pair, &end, 16);
    if (end != pair + 2) return -1;
    out[i] = (uint8_t)v;
  }
  return (int)(n / 2);
}

bool ota_begin_request(uint32_t size, const char *sha256hex, const char *sighex,
                       char *err, size_t errlen) {
  if (otaActive) { snprintf(err, errlen, "An update is already in progress"); return false; }

  // Refuse before touching flash, so a bad request costs nothing.
  if (sha256hex == nullptr || hexbytes(sha256hex, otaExpectedHash, 32) != 32) {
    snprintf(err, errlen, "Malformed sha256"); return false;
  }
  int sl = sighex ? hexbytes(sighex, otaSig, sizeof(otaSig)) : -1;
  if (sl <= 0) { snprintf(err, errlen, "Malformed signature"); return false; }
  otaSigLen = (size_t)sl;

  otaTarget = esp_ota_get_next_update_partition(NULL);
  if (otaTarget == nullptr) { snprintf(err, errlen, "No OTA partition"); return false; }
  if (size == 0 || size > otaTarget->size) {
    snprintf(err, errlen, "Image is %lu bytes; partition holds %lu",
             (unsigned long)size, (unsigned long)otaTarget->size);
    return false;
  }

  // Stop macros BEFORE anything else: the engine is loop()-task only and holds JsonObject refs
  // into the profile document. It is also the only thing in the firmware that can type, so
  // stopping it is what quiesces HID -- a half-flashed device must not reach the host. Calling
  // Keyboard.end() would additionally risk tearing down USB mid-update, for no benefit.
  macros_stop_all();
  if (macros_any_running()) { snprintf(err, errlen, "Macros still running"); return false; }

  esp_err_t rc = esp_ota_begin(otaTarget, size, &otaHandle);
  if (rc != ESP_OK) { snprintf(err, errlen, "esp_ota_begin failed (%d)", (int)rc); return false; }

  mbedtls_sha256_init(&otaSha);
  mbedtls_sha256_starts(&otaSha, 0);
  otaExpected = size;
  otaReceived = 0;
  otaLastFeedMs = millis();
  otaActive = true;

  // Idle connection parameters (50-100ms interval, slave latency 4) cost ~371ms per acked chunk
  // -- measured at ~15 minutes for a 1.23MB image against a one-minute target. Ask for fast ones
  // for the duration. Restored on EVERY exit below, because the idle settings are not a default
  // to drift back to at leisure: they exist to stop HID output stalling while idle-connected.
  ble_set_fast_conn_params(true);

  Serial.printf("[ota] begin: %lu bytes -> %s\n", (unsigned long)size, otaTarget->label);
  return true;
}

bool ota_feed(const uint8_t *data, size_t len) {
  if (!otaActive || len == 0) return otaActive;
  if (otaReceived + len > otaExpected) len = otaExpected - otaReceived;  // never overrun
  if (esp_ota_write(otaHandle, data, len) != ESP_OK) {
    Serial.println("[ota] esp_ota_write failed");
    ota_abort();
    return false;
  }
  mbedtls_sha256_update(&otaSha, data, len);
  otaReceived += len;
  otaLastFeedMs = millis();

  // LEAVE BINARY MODE THE INSTANT THE LAST BYTE LANDS.
  //
  // Without this the feature cannot work at all, and the original design missed it: the RX
  // intercept routes EVERY write to the image while otaActive is true, so the ota_end command
  // that is supposed to finish the transfer gets swallowed as image bytes, clamped to zero
  // length, and discarded. ota_finish() -- the only thing that clears otaActive -- is reachable
  // only from ota_end. A deadlock: the command needed to exit the mode is eaten by the mode.
  //
  // The device knows exactly how many bytes to expect (from ota_begin), so the last byte is an
  // unambiguous boundary. Everything after it is a command again. otaHandle, the hash and the
  // expected/received counters all stay live for ota_finish(); only the routing changes.
  if (otaReceived >= otaExpected) {
    otaActive = false;
    Serial.printf("[ota] all %lu bytes received; leaving binary mode, awaiting ota_end\n",
                  (unsigned long)otaReceived);
  }
  return true;
}

bool ota_finish(char *err, size_t errlen) {
  // Deliberately NOT gated on otaActive: ota_feed() clears that as soon as the final byte
  // arrives, which is what lets this command be delivered at all. otaHandle is the real
  // liveness signal.
  if (otaHandle == 0) { snprintf(err, errlen, "No update in progress"); return false; }

  if (otaReceived != otaExpected) {
    snprintf(err, errlen, "Got %lu of %lu bytes",
             (unsigned long)otaReceived, (unsigned long)otaExpected);
    ota_abort();
    return false;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&otaSha, digest);
  if (memcmp(digest, otaExpectedHash, 32) != 0) {
    snprintf(err, errlen, "Hash mismatch");
    ota_abort();
    return false;
  }

  // Signature over the digest. This is the gate that a hostile bonded peer cannot pass, and it
  // runs BEFORE the image is made bootable -- see the spec's ordering requirement.
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int rc = mbedtls_pk_parse_public_key(&pk, OTA_PUBKEY_DER, OTA_PUBKEY_DER_LEN);
  if (rc == 0) rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32, otaSig, otaSigLen);
  mbedtls_pk_free(&pk);
  if (rc != 0) {
    snprintf(err, errlen, "Signature rejected");
    Serial.printf("[ota] signature verify failed (mbedtls %d)\n", rc);
    ota_abort();
    return false;
  }

  if (esp_ota_end(otaHandle) != ESP_OK) {
    snprintf(err, errlen, "esp_ota_end failed");
    otaHandle = 0;                 // consumed by esp_ota_end even on failure
    otaActive = false;
    ble_set_fast_conn_params(false);
    return false;
  }
  if (esp_ota_set_boot_partition(otaTarget) != ESP_OK) {
    snprintf(err, errlen, "set_boot_partition failed");
    otaHandle = 0;
    otaActive = false;
    ble_set_fast_conn_params(false);
    return false;
  }
  otaHandle = 0;
  otaActive = false;
  ble_set_fast_conn_params(false);
  Serial.println("[ota] verified and committed; rebooting");
  return true;
}

void ota_abort() {
  if (otaHandle == 0) return;   // not otaActive: a completed-but-unfinished transfer still needs aborting
  esp_ota_abort(otaHandle);
  mbedtls_sha256_free(&otaSha);
  otaHandle = 0;
  otaActive = false;
  otaReceived = 0;
  ble_set_fast_conn_params(false);
  Serial.println("[ota] aborted; back to command mode");
}

void ota_tick() {
  // Guards on otaHandle, not otaActive. Since ota_feed() leaves binary mode on the final byte,
  // a transfer that completed but never received ota_end -- app crashed, user walked away --
  // would otherwise hold the partition handle open forever with nothing to reclaim it.
  if (otaHandle != 0 && (millis() - otaLastFeedMs) > OTA_IDLE_TIMEOUT_MS) {
    Serial.println("[ota] idle timeout");
    ota_abort();
  }
}

bool ota_confirm() {
  esp_ota_img_states_t state = ESP_OTA_IMG_VALID;
  esp_ota_get_state_partition(esp_ota_get_running_partition(), &state);
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return true;   // already settled
  bool ok = esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
  Serial.printf("[ota] confirm: %s\n", ok ? "validated" : "FAILED");
  return ok;
}
