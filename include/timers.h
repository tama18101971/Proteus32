// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file timers.h
 * @brief System tick timer (TIM2, 1 ms period)
 *
 * TIM2 is used as the system millisecond counter (analogue of Arduino millis()).
 *   - tim2_ms: global millisecond counter, incremented in IRQHandler
 *
 * Tick helper functions provide periodic flags for the main loop.
 *
 * Configuration: PSC/ARR выводятся из SYSCLK_HZ (proteus_config.h);
 * промежуточная частота таймера 1 МГц, период 1 мс.
 *
 * Power management:
 *   tim2_start() — full init + start (enables clock)
 *   tim2_stop()  — full stop + clock disable (power saving)
 */

#ifndef TIMERS_H
#define TIMERS_H

#include <stdint.h>
#include <stdbool.h>

/* ===== Public API ===== */

/**
 * @brief Initialise and start TIM2 with 1 ms period.
 */
void tim2_start(void);

/**
 * @brief Stop TIM2 and disable its clock.
 */
void tim2_stop(void);

/**
 * @brief True once every ~6.25 ms (160 Hz rate).
 */
bool tick_160hz(void);

/**
 * @brief True once every ~33.3 ms (30 Hz rate).
 */
bool tick_30hz(void);

/**
 * @brief True once every 500 ms (2 Hz rate).
 */
bool tick_2hz(void);

/**
 * @brief True once every 5 seconds.
 */
bool tick_5s(void);

/**
 * @brief Reset the 5-second timer.
 */
void tick_5s_reset(void);

/**
 * @brief Busy-wait for ms milliseconds (approximate).
 * @param ms Milliseconds to wait (0..65535).
 * Replacement for SDK Delay_Ms()/Delay_Init() — no SystemCoreClock or libgcc
 * math dependency. Loop count is derived from SYSCLK_HZ; timing is
 * approximate, not for exact measurement.
 */
void wait_ms(uint16_t ms);

extern volatile uint32_t tim2_ms; /**< Millisecond counter, incremented by TIM2 IRQ */

#endif /* TIMERS_H */
