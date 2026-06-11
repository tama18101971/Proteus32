#ifndef BUTTON_H
#define BUTTON_H

#include "ch32v00x.h"

typedef enum
{
    MODE_BEACON = 0,
    MODE_ON
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

#endif // BUTTON_H