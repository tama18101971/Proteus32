#include "dac.h"
#include "i2c.h"
#include "ch32v00x.h"
#include "config.h"

void dac_pwrpin_init(void) {
    RCC->APB2PCENR |= (1 << 5);  /* GPIOD clock enable (bit 5) */
    DAC_PWR_PORT->OUTDR &= ~DAC_PWR_PIN;
    GPIO_PinConfig(DAC_PWR_PORT, DAC_PWR_PIN, CFG_OUT_PP_2M);
}

void dac_pwrpin_on(void) {
    DAC_PWR_PORT->BSHR = DAC_PWR_PIN;
}

void dac_pwrpin_off(void) {
    DAC_PWR_PORT->BCR = DAC_PWR_PIN;
}

static void dac_write(uint8_t msb, uint8_t lsb) {
    i2c_start();
    i2c_send_addr(DAC7571_ADDRESS >> 1, I2C_DIR_TX);
    i2c_send_byte(msb);
    i2c_send_byte(lsb);
    i2c_stop();
}

void dac_set_value(uint16_t data) {
    data &= 0x0FFF;
    uint8_t msb = (uint8_t)(((DAC7571_MODE_NORMAL << 4) & 0x30) | ((data >> 8) & 0x0F));
    uint8_t lsb = (uint8_t)(data & 0xFF);
    dac_write(msb, lsb);
}

void dac_set_100K(void) {
    uint8_t msb = (uint8_t)((DAC7571_MODE_PD_100K << 4) & 0x30);
    dac_write(msb, 0x00);
}
