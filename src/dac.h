#ifndef DAC_H
#define DAC_H

#include "ch32v003_gpio.h"

/* Адрес DAC7571 (7-бит: 0x4C, для SDK нужен 8-бит адрес записи: 0x98) */
#define DAC7571_ADDRESS    0x98

/* Режимы работы / Power-Down Modes согласно даташиту DAC7571 */
#define DAC7571_MODE_NORMAL       0x00  // Нормальный режим
#define DAC7571_MODE_PD_1K        0x01  // Power-Down: 1 кОм на GND
#define DAC7571_MODE_PD_100K      0x02  // Power-Down: 100 кОм на GND
#define DAC7571_MODE_PD_HIZ       0x03  // Power-Down: High Impedance (Hi-Z)

#define DAC_PWR_PORT    GPIOD
#define DAC_PWR_PIN     GPIO_Pin_0

void dac_pwrpin_init(void);
void dac_pwrpin_on(void);
void dac_pwrpin_off(void);

void dac_set_value(uint16_t data);
void dac_set_100K(void);

#endif /* DAC_H */
