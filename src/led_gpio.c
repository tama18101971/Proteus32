/**
 * @file led_gpio.c
 * @brief GPIO LED driver for red (PC7/PC6) and green (PC4) LEDs
 *
 * LED brightness control by pin mode switching (no PWM):
 *   - on_red_h(): PC7=output-high (bright), PC6=input
 *   - on_red_l(): PC7=input-pullup (dim), PC6=output-high
 *   - off_red(): both pins output-low
 *   - on_grn_h(): PC4=output-high
 *   - on_grn_l(): PC4=input-pullup
 *   - off_grn(): PC4=output-low
 */
#include "led_gpio.h"
#include "proteus_config.h"

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
