#ifndef AWU_H
#define AWU_H

#include <stdbool.h>

typedef enum {
	AUX_15MS = 0,
	AUX_30MS,
	AUX_60MS,
	AUX_120MS,
	AUX_250MS,
	AUX_500MS,
	AUX_1S,
	AUX_2S,
	AUX_4S,
	AUX_8S,
	AUX_16S
} awu_tick_t;

void AWU_init(void);
void EXTI_INT_INIT(void);
bool awu_set_delay(awu_tick_t time_aux);

#endif // AWU_H