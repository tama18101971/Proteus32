/**
 * @file mini_gpio.h
 * @brief CH32V003 GPIO configuration helpers
 *
 * Provides pin configuration constants and a safe inline helper
 * for writing the single CFGLR register per port.
 *
 * CH32V003 has one CFGLR register per port (32 bits = 8 pins x 4 bits).
 * The SDK masks Mode via `& 0x0F`, so raw values like 0x1C are truncated.
 * Use these helpers for safe register writes.
 */

#ifndef MINI_GPIO_H
#define MINI_GPIO_H

#include "ch32v00x.h"

/** @brief Convert pin bitmask to pin number (0-7) using built-in ctz. */
#define PIN_NUM(pin_mask) (__builtin_ctz(pin_mask))

/** @brief Output push-pull 10 MHz */
#define CFG_OUT_PP_10M   0x01
/** @brief Output push-pull 2 MHz */
#define CFG_OUT_PP_2M    0x02
/** @brief Output push-pull 50 MHz */
#define CFG_OUT_PP_50M   0x03
/** @brief Input with pull-up / pull-down */
#define CFG_IN_PULL      0x08
/** @brief Floating input (high-Z) */
#define CFG_IN_FLOAT     0x04
/** @brief Analog input (ADC) */
#define CFG_IN_ANALOG    0x00

/**
 * @brief Configure a single GPIO pin.
 *
 * Writes to the port's CFGLR register, preserving other pins.
 * @param port    GPIO port (e.g. GPIOA, GPIOB, GPIOC, GPIOD).
 * @param pin_mask Pin bitmask (e.g. GPIO_Pin_0).
 * @param cfg     Configuration constant (CFG_OUT_PP_* / CFG_IN_*).
 */
static inline void GPIO_PinConfig(GPIO_TypeDef* port, uint16_t pin_mask, uint8_t cfg) {
    uint8_t pin = PIN_NUM(pin_mask);
    uint32_t shift = pin * 4;
    uint32_t mask = ~(0xF << shift);
    port->CFGLR = (port->CFGLR & mask) | ((uint32_t)cfg << shift);
}

#endif /* MINI_GPIO_H */
