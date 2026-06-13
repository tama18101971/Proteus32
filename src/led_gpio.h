#ifndef LED_GPIO_H
#define LED_GPIO_H

#include "ch32v003_gpio.h"

#define REDH_PORT	GPIOC
#define REDH_PIN	GPIO_Pin_7

#define REDL_PORT	GPIOC
#define REDL_PIN	GPIO_Pin_6

#define GRN_PORT	GPIOC
#define GRN_PIN		GPIO_Pin_4

void led_init(void);

void on_red_h(void);
void on_red_l(void);
void off_red(void);

void on_grn_h(void);
void on_grn_l(void);
void off_grn(void);

#endif // LED_GPIO_H

