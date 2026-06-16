#ifndef USER_NTC_TABLE_H_
#define USER_NTC_TABLE_H_

#include <stdint.h>

#define TEMPERATURE_TABLE_START  (-200)
#define TEMPERATURE_TABLE_STEP   (10)

int16_t calc_temperature(uint16_t adcValue);

#endif