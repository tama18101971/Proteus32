// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file button_gpio.h
 * @brief Button GPIO handler (EXTI Line3 on PC3)
 *
 * Button connected to PC3 with external pull-up.
 * Press = LOW -> EXTI Line3 falling edge.
 * In STANDBY mode the button wakes the MCU.
 *
 * In ISR: need_sleep = false and wakeup_source = WS_BUTTON. Peripheral
 * restoration remains in the main loop.
 */

#ifndef BUTTON_GPIO_H
#define BUTTON_GPIO_H

#include "ch32v00x.h"

/** @brief Wake-up source enumeration.
 *
 * packed: значения 0..2 хранятся одним байтом. Глобальная volatile
 * переменная и RAM-структуры событий не тянут 32-битный слот, а сравнения
 * по-прежнему выполняются одной инструкцией. */
typedef enum
{
    WS_NONE = 0,  /**< No wake-up */
    WS_AWU,       /**< Wake-up by AWU timer */
    WS_BUTTON     /**< Wake-up by button press */
} __attribute__((packed)) wakeup_source_t;

//extern volatile uint8_t button_event;        /**< Button press event flag */
extern volatile wakeup_source_t wakeup_source; /**< Last wake-up source */
extern volatile uint8_t need_sleep;           /**< Request to enter STANDBY */

/** @brief Initialise button GPIO and EXTI Line3. */
void button_gpio_init(void);

#endif /* BUTTON_GPIO_H */
