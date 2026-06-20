#ifndef BUTTON_GPIO_H
#define BUTTON_GPIO_H

#include "ch32v00x.h"

typedef enum
{
    WS_NONE = 0,
    WS_AWU,
    WS_BUTTON
} wakeup_source_t;

extern volatile uint8_t button_event;
extern volatile wakeup_source_t wakeup_source;

void button_init(void);

#endif // BUTTON_GPIO_H