#ifndef AWU_H
#define AWU_H

/*
 * awu.h — Auto Wake-Up таймер (AWU) для CH32V003
 *
 * AWU — аппаратный таймер, работающий от LSI (128 кГц).
 * Используется для:
 *   - Периодического пробуждения из STANDBY mode
 *   - Задержек в AUX-автомате (моргание светодиодов)
 *
 * Доступные задержки (awu_tick_t):
 *   AWU_15MS..AWU_16S — предустановленные интервалы
 *
 * Макросы:
 *   AWU_ON()  — включить AWU (PWR->AWUCSR |= AWUEN)
 *   AWU_OFF() — выключить AWU (PWR->AWUCSR &= ~AWUEN)
 *
 * Пробуждение:
 *   STANDBY → EXTI Line9 (AWU) → AWU_IRQHandler → wakeup_source = WS_AWU
 *   Кнопка  → EXTI Line3     → EXTI7_0_IRQHandler → wakeup_source = WS_BUTTON
 */

#include <stdbool.h>
#include <stdint.h>
#include "button_gpio.h"

typedef enum {
	AWU_15MS = 0,
	AWU_30MS,
	AWU_60MS,
	AWU_120MS,
	AWU_250MS,
	AWU_500MS,
	AWU_1S,
	AWU_2S,
	AWU_4S,
	AWU_8S,
	AWU_16S
} awu_tick_t;

extern volatile uint8_t awu_event;
extern uint8_t is_sleep_mode;

#define AWU_ON()    (PWR->AWUCSR |=  PWR_AWUCSR_AWUEN)  /* Включаем AWU */
#define AWU_OFF()   (PWR->AWUCSR &= ~PWR_AWUCSR_AWUEN)  /* Выключаем AWU */

void AWU_init(void);
void AWU_int_init(void);
bool AWU_set_delay(awu_tick_t time_aux);
void AWU_set_raw(uint8_t psc_idx, uint8_t win);
void AWU_stop(bool stop_lsi);
void AWU_Sleep_Tick(awu_tick_t tick);
void AWU_Sleep_Now(void);

void handle_awu(void);

#endif // AWU_H