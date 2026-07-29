/**
 * @file timers.h
 * @brief System tick timer (TIM2, 1 ms period)
 *
 * TIM2 is used as the system millisecond counter (analogue of Arduino millis()).
 *   - tim2_ms: global millisecond counter, incremented in IRQHandler
 *
 * Tick helper functions provide periodic flags for the main loop.
 *
 * Configuration: SYSCLK = 48 MHz, PSC = 47 (1 MHz), ATRLR = 999 (1 ms)
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

extern volatile uint32_t tim2_ms; /**< Millisecond counter, incremented by TIM2 IRQ */

#endif /* TIMERS_H */
