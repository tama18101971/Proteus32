// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file dac.h
 * @brief DAC7571 I2C DAC driver (12-bit)
 *
 * DAC7571 — single-channel 12-bit DAC with I2C interface.
 * Used for LED brightness control.
 *
 * I2C protocol:
 *   START -> [address][0] -> [MSB] -> [LSB] -> STOP
 *   Address: 0x4C (7-bit) / 0x98 (8-bit with R/W=0)
 *
 * Data format (16 bits after address):
 *   [15:14] — mode (00=normal, 01=PD_1K, 10=PD_100K, 11=PD_HiZ)
 *   [13:12] — reserved (00)
 *   [11:0]  — 12-bit DAC value (0x000..0xFFF)
 *
 * Power control: PD0 on GPIOD switches DAC supply.
 *
 * Return codes: I2C_OK (0) on success, I2C_NACK (1) otherwise. The compact
 * i2c driver (include/i2c.h) collapses every failure — NACK from the DAC
 * (unpowered/missing), bus error or timeout — into I2C_NACK (checks are
 * `!= I2C_OK`). On failure dac7571_set_voltage() reinitialises the I2C
 * block once (i2c_init()), so a bus timeout cannot wedge later writes.
 */

#ifndef DAC_H
#define DAC_H

#include "i2c.h"

/** @brief DAC7571 I2C 7-bit address (A0 = GND). */
#define DAC7571_I2C_ADDR    0x4C

/** @brief Maximum 12-bit DAC value. */
#define DAC_MAX_VALUE       4095

/** @brief DAC power control port (PD0). */
#define DAC_PWR_PORT    GPIOD
/** @brief DAC power control pin. */
#define DAC_PWR_PIN     GPIO_Pin_0

/* ===== Public API ===== */

/** @brief Initialise DAC power pin (PD0) as output. */
void dac_pwrpin_init(void);

/**
 * @brief Restore the DAC power-control pin after an AWU/STANDBY wake.
 *
 * Re-enables GPIOD and drives PD0 high without the low pulse performed by
 * dac_pwrpin_init().  This preserves DAC supply during an AWU-driven strobe.
 * The value cache is invalidated because the DAC rail may have dropped while
 * PD0 floated in STANDBY (hardware-dependent); the next write is forced.
 */
void dac_pwrpin_restore(void);

/** @brief Enable DAC power supply via PD0. */
void dac_pwrpin_on(void);

/** @brief Disable DAC power supply via PD0. */
void dac_pwrpin_off(void);

/**
 * @brief Set 12-bit DAC output voltage (normal mode).
 * @param data_12bit 12-bit value (0-4095).
 * @return I2C_OK or I2C_NACK.
 */
uint8_t dac7571_set_voltage(uint16_t data_12bit);

#endif /* DAC_H */
