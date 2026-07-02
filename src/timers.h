#ifndef TIMERS_H
#define TIMERS_H

/*
 * timers.h — Системный таймер TIM2 (1мс тик)
 *
 * TIM2 используется как аналог millis() в Arduino:
 *   tim2_ms — глобальный счётчик миллисекунд (инкрементируется в IRQHandler)
 *
 * Функции-таймеры (аналог millis()):
 *   tick_160hz() — true каждые ~6мс (1000/160)
 *   tick_50hz()  — true каждые ~20мс (1000/50)
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

bool tick_160hz(void);
bool tick_50hz(void);
bool tick_5s(void);
void tick_5s_reset(void);

extern volatile uint32_t tim2_ms;

#endif // TIMERS_H
