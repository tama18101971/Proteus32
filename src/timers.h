#ifndef TIMERS_H
#define TIMERS_H

/*
 * timers.h — Системный таймер TIM2 (1мс тик)
 *
 * TIM2 используется как аналог millis() в Arduino:
 *   tim2_ms — глобальный счётчик миллисекунд (инкрементируется в IRQHandler)
 *
 * Функции-таймеры (аналог millis()):
 *   tick_150hz() — true каждые ~7мс (1000/150)
 *   tick_60hz()  — true каждые ~17мс (1000/60)
 *   tick_5s()    — true каждые 5000мс
 *
 * Управление питанием:
 *   tim2_start() — полная инициализация + запуск (включает тактирование)
 *   tim2_stop()  — полная остановка + отключение тактирования (экономия энергии)
 *
 * Конфигурация: SYSCLK=48МГц, PSC=47 (1МГц), ATRLR=999 (1мс)
 */

#include <stdint.h>
#include <stdbool.h>

void tim2_start(void);
void tim2_stop(void);

bool tick_150hz(void);
bool tick_60hz(void);
bool tick_5s(void);

extern volatile uint32_t tim2_ms;

#endif // TIMERS_H
