#ifndef BUTTON_H
#define BUTTON_H

#include "ch32v00x.h"

typedef enum {
	CMODE_OFF =	0U,
	CMODE_MOON,
	CMODE_MIN,
	CMODE_MID0,
	CMODE_MID,
	CMODE_MAX,
	CMODE_TURBO,
	CMODE_NOACTION
} CURRENT_Modes_t;


#define BASE_MOON_LEVEL 4
#define DEFAULT_STEP 500			// Шаг для УИ1.  Применяется при начальной инициализации.
#define DEFAULT_MODE CMODE_MID		// Моде для УИ2. Применяется при начальной инициализации.


typedef enum
{
    WS_AWU = 0,
    WS_BUTTON
} wakeup_source_t;

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