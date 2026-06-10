#ifndef AWU_H
#define AWU_H

typedef enum {
	AUX_15MS,
	AUX_30MS,
	AUX_60MS,
	AUX_120MS,
	AUX_250MS,
	AUX_500MS,
	AUX_1S,
	AUX_2S,
	AUX_4S,
	AUX_8S,
	AUX_16S,
	AUX_32S,
	AUX_1M,
	AUX_5M
} awu_tick_t;

void AWU_init(void);
void awu_set_delay(awu_tick_t time_aux);

#endif // AWU_H