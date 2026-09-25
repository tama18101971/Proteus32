/**
 * @file awu.h
 * @brief Auto Wake-Up timer (AWU) for CH32V003
 *
 * AWU is a hardware timer running from LSI (128 kHz).
 * Used for:
 *   - Periodic wake-up from STANDBY mode
 *   - Delays in the AUX state machine (LED blinking)
 *
 * Wake-up sources:
 *   STANDBY -> EXTI Line9 (AWU) -> AWU_IRQHandler -> wakeup_source = WS_AWU
 *   Button  -> EXTI Line3      -> EXTI7_0_IRQHandler -> wakeup_source = WS_BUTTON
 */

#ifndef AWU_H
#define AWU_H

#include <stdbool.h>
#include <stdint.h>
#include "button_gpio.h"
#include "proteus_config.h"

#ifdef UART_DEBUG
/**
 * @brief Ограниченное ожидание завершения передачи UART (флаг TC USART1).
 *
 * Общая для ISR кнопки и входа в STANDBY: сбрасывает последний байт перед
 * потерей тактирования USART1; при неисправном USART выходит по таймауту.
 */
void uart_tc_wait(void);
#endif

/** @brief Preset AWU delay intervals (index into awu_lut[] in awu.c).
 *
 * Only the intervals the firmware actually arms are listed: every entry costs
 * 2 bytes of flash.  AWU_set_delay() takes no range check, so the enum and
 * awu_lut[] must stay in lockstep — AWU_TICKS_LAST is pinned against the table
 * length by a _Static_assert in awu.c, so a preset added to this enum without a
 * matching awu_lut[] row fails the build. */
typedef enum {
	AWU_15MS = 0,  /**< 15 ms  = (128  * 15) / 128000 */
	AWU_60MS,      /**< 60 ms  = (256  * 30) / 128000 */
	AWU_120MS,     /**< 120 ms = (256  * 60) / 128000 */
	AWU_1S,        /**< 1 s    = (2048 * 63) / 128000 */
	AWU_4S,        /**< 4 s    = (10240 * 50) / 128000 */
	AWU_TICKS_LAST /**< Sentinel: not a preset; must equal AWU_TICKS_COUNT. */
} awu_tick_t;

/**
 * @brief Start an AUX command immediately, without an initial AWU delay.
 *
 * Sentinel value for start_aux_cmd()/start_aux_array(): it must not be
 * passed to AWU_set_delay() because it is not a hardware AWU interval.
 * Small and outside the preset enum on purpose: every `li` of it in call
 * sites is emitted as a compressed c.li, saving Flash on RV32EC.
 */
#define AWU_IMMEDIATELY ((awu_tick_t)0x0CU)

//extern volatile uint8_t awu_event; /**< AWU event flag, set in ISR */
extern volatile uint8_t is_sleep_mode; /**< Non-zero when MCU is in STANDBY */

#if USE_LSI_GUARD
/** @brief LSI stability result of the last AWU_init() (see awu.c). */
extern bool awu_ready;
#endif

/** @brief Enable AWU peripheral. */
#define AWU_ON()    (PWR->AWUCSR |=  PWR_AWUCSR_AWUEN)
/** @brief Disable AWU peripheral. */
#define AWU_OFF()   (PWR->AWUCSR &= ~PWR_AWUCSR_AWUEN)

/* ===== Public API ===== */

/**
 * @brief Initialise AWU peripheral (clock, reset).
 */
void AWU_init(void);

/**
 * @brief Initialise AWU interrupt (NVIC, EXTI Line9).
 */
void AWU_int_init(void);

/**
 * @brief Set AWU delay from preset table.
 * @param time_aux Requested delay interval (awu_tick_t preset).
 */
void AWU_set_delay(awu_tick_t time_aux);

/**
 * @brief Set AWU prescaler and window directly.
 * @param psc_idx Prescaler index (0-7).
 * @param win     Window counter value.
 */
void AWU_set_raw(uint8_t psc_idx, uint8_t win);

/**
 * @brief Stop AWU and optionally stop LSI.
 * @param stop_lsi If true, also disable the LSI oscillator.
 */
void AWU_stop(bool stop_lsi);

/**
 * @brief Enter STANDBY and wait for wake-up.
 *
 * Wake-up source is returned through the global `wakeup_source`
 * (WS_NONE, WS_BUTTON or WS_AWU).
 */
void AWU_Sleep_Now(void);

#endif /* AWU_H */
