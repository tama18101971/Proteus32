/**
 * @file button_gpio.h
 * @brief Button GPIO handler (EXTI Line3 on PC3)
 *
 * Button connected to PC3 with external pull-up.
 * Press = LOW -> EXTI Line3 falling edge.
 * In STANDBY mode the button wakes the MCU.
 *
 * In ISR: wakeup_source = WS_BUTTON, need_sleep = false.
 */

#ifndef BUTTON_GPIO_H
#define BUTTON_GPIO_H

#include "ch32v00x.h"

/** @brief Wake-up source enumeration. */
typedef enum
{
    WS_NONE = 0,  /**< No wake-up */
    WS_AWU,       /**< Wake-up by AWU timer */
    WS_BUTTON     /**< Wake-up by button press */
} wakeup_source_t;

extern volatile uint8_t button_event;        /**< Button press event flag */
extern volatile wakeup_source_t wakeup_source; /**< Last wake-up source */
extern uint8_t need_sleep;                    /**< Request to enter STANDBY */

/** @brief Initialise button GPIO and EXTI Line3. */
void button_gpio_init(void);

#endif /* BUTTON_GPIO_H */