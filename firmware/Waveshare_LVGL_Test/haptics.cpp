#include "haptics.h"
#include <Arduino.h>
#include "driver/i2c.h"
#include "lcd_config.h"

// Same bus/port cst816.cpp installs in Touch_Init() (SDA 11 / SCL 12). We deliberately do NOT
// call i2c_driver_install() here -- doing it twice on one port fails, and the touch controller
// owns the bus setup.
#define HAPTICS_I2C_PORT I2C_NUM_0
#define I2C_TIMEOUT_TICKS pdMS_TO_TICKS(50)

// Standard DRV2605 7-bit address. Fixed in silicon -- the part has no address-select pin -- so
// if this does not answer, the chip is either absent, unpowered, or on a different bus.
#define DRV2605_ADDR 0x5A

#define DRV2605_REG_MODE       0x01
#define DRV2605_REG_RTPIN      0x02
#define DRV2605_REG_LIBRARY    0x03
#define DRV2605_REG_WAVESEQ1   0x04
#define DRV2605_REG_WAVESEQ2   0x05
#define DRV2605_REG_GO         0x0C
#define DRV2605_REG_OVERDRIVE  0x0D
#define DRV2605_REG_SUSTAINPOS 0x0E
#define DRV2605_REG_SUSTAINNEG 0x0F
#define DRV2605_REG_BREAK      0x10
#define DRV2605_REG_AUDIOMAX   0x13
#define DRV2605_REG_FEEDBACK   0x1A
#define DRV2605_REG_CONTROL3   0x1D

// Effect 1 in the DRV2605's ROM library: "Strong Click - 100%". Short and crisp, which is what a
// confirmation wants -- a long buzz reads as an error rather than an acknowledgement.
#define HAPTIC_EFFECT_STRONG_CLICK 1

static bool s_available = false;

static bool drv_write(uint8_t reg, uint8_t value) {
  uint8_t buf[2] = { reg, value };
  return i2c_master_write_to_device(HAPTICS_I2C_PORT, DRV2605_ADDR, buf, sizeof(buf),
                                    I2C_TIMEOUT_TICKS) == ESP_OK;
}

static bool drv_read(uint8_t reg, uint8_t *value) {
  return i2c_master_write_read_device(HAPTICS_I2C_PORT, DRV2605_ADDR, &reg, 1, value, 1,
                                      I2C_TIMEOUT_TICKS) == ESP_OK;
}

// Probe every 7-bit address and log what answers. Cheap (one zero-length write each) and run
// once at boot, and it is the only way we learn what is actually on this bus -- the DRV2605
// address here is an assumption from the datasheet, not from a verified schematic.
static void i2c_scan(void) {
  Serial.print("[haptics] I2C scan:");
  int found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    uint8_t dummy = 0;
    if (i2c_master_write_to_device(HAPTICS_I2C_PORT, addr, &dummy, 0, I2C_TIMEOUT_TICKS) == ESP_OK) {
      Serial.printf(" 0x%02X", addr);
      found++;
    }
  }
  if (found == 0) Serial.print(" (nothing responded)");
  Serial.println();
}

void haptics_init() {
  i2c_scan();

  uint8_t status = 0;
  if (!drv_read(0x00, &status)) {
    Serial.println("[haptics] DRV2605 not responding at 0x5A -- haptics disabled");
    s_available = false;
    return;
  }
  Serial.printf("[haptics] DRV2605 found at 0x5A, status=0x%02X\n", status);

  // Out of standby, internal trigger: playback starts when we write GO, not from an external pin.
  drv_write(DRV2605_REG_MODE, 0x00);
  drv_write(DRV2605_REG_RTPIN, 0x00);

  // Zero the envelope registers -- library effects carry their own shape, and leftover overdrive
  // or sustain values distort them.
  drv_write(DRV2605_REG_OVERDRIVE, 0x00);
  drv_write(DRV2605_REG_SUSTAINPOS, 0x00);
  drv_write(DRV2605_REG_SUSTAINNEG, 0x00);
  drv_write(DRV2605_REG_BREAK, 0x00);
  drv_write(DRV2605_REG_AUDIOMAX, 0x64);

  // ERM open-loop, library A. ERM (eccentric rotating mass) is the assumption: it suits the small
  // coin motors these boards usually carry, and open loop avoids needing an auto-calibration pass.
  //
  // If the motor turns out to be an LRA, this drives it weakly or not at all rather than damaging
  // anything -- the fix is to set the FEEDBACK N_ERM_LRA bit (0x80) and use library 6. Judge by
  // whether the buzz is clearly felt.
  uint8_t feedback = 0;
  if (drv_read(DRV2605_REG_FEEDBACK, &feedback)) {
    drv_write(DRV2605_REG_FEEDBACK, feedback & 0x7F); // N_ERM_LRA = 0 -> ERM
  }
  uint8_t control3 = 0;
  if (drv_read(DRV2605_REG_CONTROL3, &control3)) {
    drv_write(DRV2605_REG_CONTROL3, control3 | 0x20); // ERM_OPEN_LOOP
  }
  drv_write(DRV2605_REG_LIBRARY, 1);

  drv_write(DRV2605_REG_WAVESEQ1, HAPTIC_EFFECT_STRONG_CLICK);
  drv_write(DRV2605_REG_WAVESEQ2, 0); // end of sequence

  s_available = true;
  Serial.println("[haptics] configured (ERM open-loop, library 1, effect 1)");
}

void haptics_pulse() {
  if (!s_available) return;
  // The waveform sequence is already loaded from init and never changes, so firing is a single
  // register write -- cheap enough to call from a UI callback without queueing it to loop().
  drv_write(DRV2605_REG_GO, 0x01);
}

bool haptics_available() {
  return s_available;
}
