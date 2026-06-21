#ifndef ADC_H_
#define ADC_H_

#include <stdint.h>

#define TEMPERATURE_TABLE_START  (-200)
#define TEMPERATURE_TABLE_STEP   (10)

void adc_read(void);
void adc_init(void);
void adc_stop(void);

int16_t calc_temperature(uint16_t adcValue);

/* EMA-фильтр для Vrefint ADC (измерение напряжения Vdd).
 * shift=4: alpha=1/16, запаздывание ~16 отсчётов.
 * shift=3: alpha=1/8,  запаздывание ~8 отсчётов.
 * Больше shift — плавнее, но медленнее реакция. */
#define EMA_SHIFT 4
void     ema_init(uint16_t seed);
uint16_t ema_update(uint16_t raw);

#endif /* ADC_H_ */