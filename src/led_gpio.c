#include "led_gpio.h"
#include "ch32v00x.h"

#include <stdint.h>

#define CFG_OUT_PP_10M   0x01
#define CFG_IN_PULL      0x08
#define CFG_IN_FLOAT     0x04

#define PIN_NUM(pin_mask) (__builtin_ctz(pin_mask))

static inline void GPIO_PinConfig(GPIO_TypeDef* port, uint16_t pin_mask, uint8_t cfg) {
    uint8_t pin = PIN_NUM(pin_mask);
    uint32_t shift = pin * 4;
    uint32_t mask = ~(0xF << shift);
    port->CFGLR = (port->CFGLR & mask) | ((uint32_t)cfg << shift);
}

/*********************************************************************
 * LED PC4, PC7 - выход. PC6 - вход
 *********************************************************************/
void led_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;

    REDH_PORT->BCR = REDH_PIN;
    GRN_PORT->BCR = GRN_PIN;

    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_OUT_PP_10M);
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_OUT_PP_10M);

    GPIO_PinConfig(REDL_PORT, REDL_PIN, CFG_IN_FLOAT);
}

void on_red_h(void) {
    REDL_PORT->BCR = REDL_PIN;

    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_OUT_PP_10M);
    REDH_PORT->BSHR = REDH_PIN;
}

void on_red_l(void) {
    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_IN_PULL);
    REDH_PORT->BSHR = REDH_PIN;

    REDL_PORT->BSHR = REDL_PIN;
}

void off_red(void) {
    REDL_PORT->BCR = REDL_PIN;

    REDH_PORT->BCR = REDH_PIN;
    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_OUT_PP_10M);
}

void on_grn_h(void) {
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_OUT_PP_10M);
    GRN_PORT->BSHR = GRN_PIN;
}

void on_grn_l(void) {
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_IN_PULL);
    GRN_PORT->BSHR = GRN_PIN;
}

void off_grn(void) {
    GRN_PORT->BCR = GRN_PIN;
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_OUT_PP_10M);
}
