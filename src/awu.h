#ifndef AWU_H
#define AWU_H

#include <stdbool.h>

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

void AWU_init(void);
void AWU_int_init(void);
bool AWU_set_delay(awu_tick_t time_aux);
void AWU_stop(bool stop_lsi);
void AWU_Sleep_Tick(awu_tick_t tick);
void AWU_Sleep_Now(void);

#endif // AWU_H