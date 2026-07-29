/**
 * @file led_gpio.h
 * @brief GPIO LED control (no PWM)
 *
 * Pin mapping:
 *   PC7 — Red LED high-side
 *   PC6 — Red LED low-side
 *   PC4 — Green LED
 *
 * Brightness is controlled by toggling pin modes
 * (output-high, input-pullup, output-low) — no PWM.
 */

#ifndef LED_GPIO_H
#define LED_GPIO_H

#include "mini_gpio.h"

/** @brief Red LED high-side port/pin (PC7). */
#define REDH_PORT	GPIOC
#define REDH_PIN	GPIO_Pin_7

/** @brief Red LED low-side port/pin (PC6). */
#define REDL_PORT	GPIOC
#define REDL_PIN	GPIO_Pin_6

/** @brief Green LED port/pin (PC4). */
#define GRN_PORT	GPIOC
#define GRN_PIN		GPIO_Pin_4

/* ===== Public API ===== */

/** @brief Initialise all LED GPIO pins. */
void led_init(void);

/** @brief Red LED bright (high-side on, low-side off). */
void on_red_h(void);
/** @brief Red LED dim (low-side on, high-side input). */
void on_red_l(void);
/** @brief Red LED off. */
void off_red(void);

/** @brief Green LED on (bright). */
void on_grn_h(void);
/** @brief Green LED dim. */
void on_grn_l(void);
/** @brief Green LED off. */
void off_grn(void);

#endif /* LED_GPIO_H */

