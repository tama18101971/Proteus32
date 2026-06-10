#ifndef LED_GPIO_H
#define LED_GPIO_H

#include "ch32v003_gpio.h"

#define REDH_PORT	GPIOC
#define REDH_PIN	GPIO_Pin_7

#define REDL_PORT	GPIOC
#define REDL_PIN	GPIO_Pin_6

#define GRN_PORT	GPIOC
#define GRN_PIN		GPIO_Pin_4

// Удобные макросы для конфигурации (4-битные поля)
#define CFG_OUT_PP_10M   0x01  // Выход Push-Pull, 10 МГц
#define CFG_OUT_PP_2M    0x02  // Выход Push-Pull, 2 МГц
#define CFG_OUT_PP_50M   0x03  // Выход Push-Pull, 50 МГц
#define CFG_IN_PULL      0x08  // Вход с Pull-up / Pull-down
#define CFG_IN_FLOAT     0x04  // Вход плавающий
#define CFG_IN_ANALOG    0x00  // Аналоговый вход

void led_init(void);

void on_red_h(void);
void on_red_l(void);
void off_red(void);

void on_grn_h(void);
void on_grn_l(void);
void off_grn(void);

#endif // LED_GPIO_H

