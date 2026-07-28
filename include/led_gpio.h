#ifndef LED_GPIO_H
#define LED_GPIO_H

/*
 * led_gpio.h — Управление GPIO для светодиодов
 *
 * Пин-маппинг (CH32V003):
 *   PC7 — Красный LED (высокий уровень)
 *   PC6 — Красный LED (низкий уровень)
 *   PC4 — Зелёный LED
 *
 * Функции:
 *   led_init()   — инициализация GPIO для LED
 *   on_red_h()   — включить красный (ярко)
 *   on_red_l()   — включить красный (слабо)
 *   off_red()    — выключить красный
 *   on_grn_h()   — включить зелёный (ярко)
 *   on_grn_l()   — включить зелёный (слабо)
 *   off_grn()    — выключить зелёный
 */

#include "ch32v00x.h"

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

