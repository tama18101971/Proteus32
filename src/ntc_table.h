#ifndef USER_NTC_TABLE_H_
#define USER_NTC_TABLE_H_

#include <stdint.h>

#define TEMPERATURE_TABLE_START  (-200)
#define TEMPERATURE_TABLE_STEP   (10)

void ntc_read(void);
void ntc_adc_Init(void);

int16_t calc_temperature(uint16_t adcValue);

#endif