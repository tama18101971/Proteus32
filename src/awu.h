#ifndef AWU_H
#define AWU_H

#include <stdbool.h>
#include <stdint.h>
#include "button_gpio.h"

typedef enum {
	AWU_15MS = 0,
	AWU_30MS,
	AWU_60MS,
	AWU_120MS,
	AWU_250MS,
	AWU_500MS,
	AWU_1S,
	AWU_2S,
	AWU_4S,
	AWU_8S,
	AWU_16S
} awu_tick_t;

extern volatile uint8_t awu_event;

#define AWU_ON()    (PWR->AWUCSR |=  PWR_AWUCSR_AWUEN)  /* Включаем AWU */
#define AWU_OFF()   (PWR->AWUCSR &= ~PWR_AWUCSR_AWUEN)  /* Выключаем AWU */

void AWU_init(void);
void AWU_int_init(void);
bool AWU_set_delay(awu_tick_t time_aux);
void AWU_set_raw(uint8_t psc_idx, uint8_t win);
void AWU_stop(bool stop_lsi);
void AWU_Sleep_Tick(awu_tick_t tick);
wakeup_source_t AWU_Sleep_Now(void);

void handle_awu(void);

#endif // AWU_H