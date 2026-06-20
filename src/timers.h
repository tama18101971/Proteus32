#ifndef TIMERS_H
#define TIMERS_H

#include <stdint.h>

void tim2_start(void);
void tim2_stop(void);

extern volatile uint32_t tim2_ms;

#endif // TIMERS_H
