#ifndef LIMITERS_H
#define LIMITERS_H

/*
 * limiters.h — Заголовочный файл для модуля ограничителей (limiters)
 *
 */

#include "thermal_limiter.h"

#define TERMO_50C_ADC    722u  /* termo_table[70] = ADC-код при 50°C */
#define TCALIBR_DEFAULT  6u    /* default EE.usr.t_calibr */
#define BASE_TEMPERATURE  (TERMO_50C_ADC - TCALIBR_DEFAULT)

void sys_init(void);
void scan_limiters(void);
void reset_limiters(void);

extern uint16_t limiter;
extern ThermalLimiterState_t  state;
extern const ThermalLimiterConfig_t config;

#endif /* LIMITERS_H */