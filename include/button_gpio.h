#ifndef BUTTON_GPIO_H
#define BUTTON_GPIO_H

/*
 * button_gpio.h — Обработчик кнопки (EXTI Line3 на PC3)
 *
 * Кнопка подключена к PC3 с pull-up.
 * Нажатие = LOW → EXTI Line3 по спадающему фронту.
 *
 * В режиме сна (STANDBY) кнопка пробуждает MCU.
 * В ISR: wakeup_source = WS_BUTTON, need_sleep = false.
 *
 * Типы пробуждения (wakeup_source_t):
 *   WS_NONE   — не было пробуждения
 *   WS_AWU    — пробуждение по AWU таймеру
 *   WS_BUTTON — пробуждение по кнопке
 */

#include "ch32v00x.h"

typedef enum
{
    WS_NONE = 0,
    WS_AWU,
    WS_BUTTON
} wakeup_source_t;

extern volatile uint8_t button_event;
extern volatile wakeup_source_t wakeup_source;
extern uint8_t need_sleep;

void button_gpio_init(void);

#endif // BUTTON_GPIO_H