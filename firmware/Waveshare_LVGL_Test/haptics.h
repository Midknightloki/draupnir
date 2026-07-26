#pragma once
#include <stdint.h>

// Haptic feedback via the board's DRV2605 vibration-motor driver.
//
// This replaces the M5Dial build's buzzer blip: the Waveshare knob has no buzzer, but it does
// carry a DRV2605 haptic driver on the I2C bus, so confirmation feedback is a short buzz instead
// of a beep. The point is the same -- eyes-free confirmation that a gesture registered.
//
// Every entry point is a safe no-op when the chip is not found, so a wrong address or a board
// revision without the driver degrades to "no haptics" rather than blocking boot.

// Probes for the DRV2605 and configures it for waveform playback. Must be called AFTER
// Touch_Init(), which is what installs the shared I2C driver this reuses -- calling it earlier
// fails on an uninstalled bus.
//
// Also logs a full I2C bus scan. The DRV2605's address on this board is assumed, not confirmed
// from a schematic, so the scan is how we learn the truth on first boot: if 0x5A is absent the
// log still shows every device that did answer.
void haptics_init();

// Short confirmation buzz. Safe to call from any task -- a single I2C transaction, no blocking
// waits beyond the bus timeout, and a no-op if the chip is absent.
void haptics_pulse();

bool haptics_available();
