#ifndef LIMITERS_H
#define LIMITERS_H

/*
 * limiters.h — Заголовочный файл для модуля ограничителей (limiters)
 *
 */
#include "pi_reg.h"


void sys_init(void);
void scan_limiters(void);
void reset_limiters(void);

extern uint16_t limiter;
extern pi_reg_debug_t dbg_data;

#endif /* LIMITERS_H */