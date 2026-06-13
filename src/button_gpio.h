#ifndef BUTTON_GPIO_H
#define BUTTON_GPIO_H

#include "ch32v00x.h"

typedef enum
{
    WS_AWU = 0,
    WS_BUTTON
} wakeup_source_t;

typedef enum
{
    MODE_BEACON = 0,
    MODE_ON1
} driver_mode_t;

typedef enum
{
    LED_OFF_STATE = 0,
    LED_ON_STATE
} led_state_t;

extern volatile uint8_t button_event;
extern volatile driver_mode_t mode;
extern led_state_t led_state;

void Button_Init(void);

#endif // BUTTON_GPIO_H