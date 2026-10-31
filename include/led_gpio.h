// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file led_gpio.h
 * @brief GPIO LED control (no PWM)
 *
 * Pin mapping (default, USE_IND_RGB=0):
 *   PC7 — Red LED high-side
 *   PC6 — Red LED low-side
 *   PC4 — Green LED
 *
 * Pin mapping (USE_IND_RGB=1):
 *   PC7 — Red LED
 *   PC4 — Green LED
 *   PC6 — Blue LED
 *
 * Brightness is controlled by toggling pin modes
 * (output-high, input-pullup, output-low) — no PWM.
 */

#ifndef LED_GPIO_H
#define LED_GPIO_H

#include "mini_gpio.h"
#include "proteus_config.h"

/** @brief Red LED port/pin (PC7; в дефолтной плате — high-side). */
#define REDH_PORT	GPIOC
#define REDH_PIN	GPIO_Pin_7

#if USE_IND_RGB
/** @brief Blue LED port/pin (PC6; в дефолтной плате — low-side красного). */
#define BLUE_PORT	GPIOC
#define BLUE_PIN	GPIO_Pin_6
#else
/** @brief Red LED low-side port/pin (PC6). */
#define REDL_PORT	GPIOC
#define REDL_PIN	GPIO_Pin_6
#endif

/** @brief Green LED port/pin (PC4). */
#define GRN_PORT	GPIOC
#define GRN_PIN		GPIO_Pin_4

/* ===== Public API ===== */

/** @brief Initialise all LED GPIO pins. */
void led_init(void);

/** @brief Red LED bright. */
void on_red_h(void);
/** @brief Red LED dim. */
void on_red_l(void);
/** @brief Red LED off. */
void off_red(void);

/** @brief Green LED on (bright). */
void on_grn_h(void);
/** @brief Green LED dim. */
void on_grn_l(void);
/** @brief Green LED off. */
void off_grn(void);

#if USE_IND_RGB
/** @brief Blue LED on (bright). */
void on_blue_h(void);
/** @brief Blue LED dim. */
void on_blue_l(void);
/** @brief Blue LED off. */
void off_blue(void);
#endif

#endif /* LED_GPIO_H */

