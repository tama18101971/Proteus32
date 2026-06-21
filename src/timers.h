#ifndef TIMERS_H
#define TIMERS_H

#include <stdint.h>
#include <stdbool.h>

void tim2_start(void);
void tim2_stop(void);

bool tick_150hz(void);
bool tick_60hz(void);
bool tick_5s(void);

extern volatile uint32_t tim2_ms;

#endif // TIMERS_H
