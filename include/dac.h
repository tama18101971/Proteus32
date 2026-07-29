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
 * Return codes: I2C_OK (0) on ACK, I2C_NACK (1) on NACK (see i2c.h).
 */

#ifndef DAC_H
#define DAC_H

#include "i2c.h"

/** @brief DAC7571 I2C 7-bit address (A0 = GND). */
#define DAC7571_I2C_ADDR    0x4C

/** @brief Maximum 12-bit DAC value. */
#define DAC_MAX_VALUE       4095

/** @brief Normal operating mode (active output). */
#define DAC7571_MODE_NORMAL    0x00
/** @brief Power-down: 1 kOhm to GND. */
#define DAC7571_MODE_PD_1K     0x01
/** @brief Power-down: 100 kOhm to GND. */
#define DAC7571_MODE_PD_100K   0x02
/** @brief Power-down: High Impedance. */
#define DAC7571_MODE_PD_HIZ    0x03

/** @brief DAC power control port (PD0). */
#define DAC_PWR_PORT    GPIOD
/** @brief DAC power control pin. */
#define DAC_PWR_PIN     GPIO_Pin_0

/* ===== Public API ===== */

/** @brief Initialise DAC power pin (PD0) as output. */
void dac_pwrpin_init(void);

/** @brief Enable DAC power supply via PD0. */
void dac_pwrpin_on(void);

/** @brief Disable DAC power supply via PD0. */
void dac_pwrpin_off(void);

/**
 * @brief Check if DAC power is on.
 * @return 1 if powered, 0 if off.
 */
uint8_t dac_pwrpin_is_on(void);

/**
 * @brief Set 12-bit DAC output voltage (normal mode).
 * @param data_12bit 12-bit value (0-4095).
 * @return I2C_OK or I2C_NACK.
 */
uint8_t dac7571_set_voltage(uint16_t data_12bit);

/**
 * @brief Enter power-down mode.
 * @param mode Power-down mode (DAC7571_MODE_PD_*).
 * @return I2C_OK or I2C_NACK.
 */
uint8_t dac7571_power_down(uint8_t mode);

#endif /* DAC_H */
