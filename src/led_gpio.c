// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file led_gpio.c
 * @brief GPIO LED driver: default — red (PC7/PC6) + green (PC4);
 *        USE_IND_RGB — red (PC7), green (PC4), blue (PC6)
 *
 * LED brightness control by pin mode switching (no PWM):
 *   - bright:  pin output-high (push-pull)
 *   - dim:     pin input-pullup
 *   - off:     pin output-low
 * В дефолтной плате красный двухпиновый (high-side PC7 / low-side PC6):
 *   - on_red_h(): PC7=output-high (bright), PC6=input
 *   - on_red_l(): PC7=input-pullup (dim), PC6=input-pullup
 *   - off_red():  PC7=output-low, PC6=input
 * В RGB-плате каждый канал однопиновый (как зелёный), PC6 = blue.
 */
#include "led_gpio.h"
#include "proteus_config.h"

#if USE_IND_RGB

/** @brief Initialise RGB LED GPIO pins (PC7/PC4/PC6, all off = output-low) and unused pins to Hi-Z Analog. */
void led_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOD;

    /* Unused pins to Analog Input (0x00) to eliminate floating CMOS input leakage */
    GPIO_PinConfig(GPIOA, GPIO_Pin_1, CFG_IN_ANALOG);
    GPIO_PinConfig(GPIOA, GPIO_Pin_2, CFG_IN_ANALOG);
    GPIO_PinConfig(GPIOC, GPIO_Pin_0, CFG_IN_ANALOG);
    GPIO_PinConfig(GPIOC, GPIO_Pin_5, CFG_IN_ANALOG);
    GPIO_PinConfig(GPIOD, GPIO_Pin_5, CFG_IN_ANALOG);
    GPIO_PinConfig(GPIOD, GPIO_Pin_6, CFG_IN_ANALOG);
#ifndef UART_DEBUG
    GPIO_PinConfig(GPIOD, GPIO_Pin_4, CFG_IN_ANALOG);
#endif

    REDH_PORT->BCR = REDH_PIN;
    GRN_PORT->BCR = GRN_PIN;
    BLUE_PORT->BCR = BLUE_PIN;

    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_OUT_PP_10M);
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_OUT_PP_10M);
    GPIO_PinConfig(BLUE_PORT, BLUE_PIN, CFG_OUT_PP_10M);
}

/** @brief Red LED high (bright). */
void on_red_h(void) {
    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_OUT_PP_10M);
    REDH_PORT->BSHR = REDH_PIN;
}

/** @brief Red LED low (dim). */
void on_red_l(void) {
    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_IN_PULL);
    REDH_PORT->BSHR = REDH_PIN;
}

/** @brief Red LED off. */
void off_red(void) {
    REDH_PORT->BCR = REDH_PIN;
    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_OUT_PP_10M);
}

/** @brief Blue LED high (bright). */
void on_blue_h(void) {
    GPIO_PinConfig(BLUE_PORT, BLUE_PIN, CFG_OUT_PP_10M);
    BLUE_PORT->BSHR = BLUE_PIN;
}

/** @brief Blue LED low (dim). */
void on_blue_l(void) {
    GPIO_PinConfig(BLUE_PORT, BLUE_PIN, CFG_IN_PULL);
    BLUE_PORT->BSHR = BLUE_PIN;
}

/** @brief Blue LED off. */
void off_blue(void) {
    BLUE_PORT->BCR = BLUE_PIN;
    GPIO_PinConfig(BLUE_PORT, BLUE_PIN, CFG_OUT_PP_10M);
}

#else  /* !USE_IND_RGB — дефолтная двухпиновая красная схема, без изменений */

/** @brief Initialise all LED GPIO pins (red high/low-side, green) and unused pins to Hi-Z Analog. */
void led_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOD;

    /* Unused pins to Analog Input (0x00) to eliminate floating CMOS input leakage */
    GPIO_PinConfig(GPIOA, GPIO_Pin_1, CFG_IN_ANALOG);
    GPIO_PinConfig(GPIOA, GPIO_Pin_2, CFG_IN_ANALOG);
    GPIO_PinConfig(GPIOC, GPIO_Pin_0, CFG_IN_ANALOG);
    GPIO_PinConfig(GPIOC, GPIO_Pin_5, CFG_IN_ANALOG);
    GPIO_PinConfig(GPIOD, GPIO_Pin_5, CFG_IN_ANALOG);
    GPIO_PinConfig(GPIOD, GPIO_Pin_6, CFG_IN_ANALOG);
#ifndef UART_DEBUG
    GPIO_PinConfig(GPIOD, GPIO_Pin_4, CFG_IN_ANALOG);
#endif

    REDH_PORT->BCR = REDH_PIN;
    GRN_PORT->BCR = GRN_PIN;
    REDL_PORT->BCR = REDL_PIN;

    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_OUT_PP_10M);
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_OUT_PP_10M);

    GPIO_PinConfig(REDL_PORT, REDL_PIN, CFG_IN_FLOAT);
}

/** @brief Red LED high (bright). */
void on_red_h(void) {
    REDL_PORT->BCR = REDL_PIN;
    GPIO_PinConfig(REDL_PORT, REDL_PIN, CFG_IN_FLOAT);

    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_OUT_PP_10M);
    REDH_PORT->BSHR = REDH_PIN;
}

/** @brief Red LED low (dim). */
void on_red_l(void) {
    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_IN_PULL);
    REDH_PORT->BSHR = REDH_PIN;

    GPIO_PinConfig(REDL_PORT, REDL_PIN, CFG_IN_PULL);
    REDL_PORT->BSHR = REDL_PIN;
}

/** @brief Red LED off. */
void off_red(void) {
    REDL_PORT->BCR = REDL_PIN;
    GPIO_PinConfig(REDL_PORT, REDL_PIN, CFG_IN_FLOAT);

    REDH_PORT->BCR = REDH_PIN;
    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_OUT_PP_10M);
}

#endif /* USE_IND_RGB */

/** @brief Green LED high (bright). */
void on_grn_h(void) {
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_OUT_PP_10M);
    GRN_PORT->BSHR = GRN_PIN;
}

/** @brief Green LED low (dim). */
void on_grn_l(void) {
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_IN_PULL);
    GRN_PORT->BSHR = GRN_PIN;
}

/** @brief Green LED off. */
void off_grn(void) {
    GRN_PORT->BCR = GRN_PIN;
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_OUT_PP_10M);
}
