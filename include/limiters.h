#ifndef LIMITERS_H
#define LIMITERS_H

/*
 * limiters.h — Заголовочный файл для модуля ограничителей (limiters)
 *
 */

#include "thermal_limiter.h"

#define T_SET 700u  /* Заданная температура (код АЦП) — ЗАМЕРИТЬ на реальном
                       железе при желаемой предельной температуре, см. TUNING.md */

void sys_init(void);
void scan_limiters(void);
void reset_limiters(void);

extern uint16_t limiter;
extern ThermalLimiterState_t  state;
extern const ThermalLimiterConfig_t config;

#endif /* LIMITERS_H */