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

/** @brief Preset AWU delay intervals. */
typedef enum {
	AWU_1MS = 0,  /**< 1 ms */
	AWU_15MS,     /**< 15 ms */
	AWU_30MS,     /**< 30 ms */
	AWU_60MS,     /**< 60 ms */
	AWU_120MS,    /**< 120 ms */
	AWU_250MS,    /**< 250 ms */
	AWU_500MS,    /**< 500 ms */
	AWU_1S,       /**< 1 second */
	AWU_2S,       /**< 2 seconds */
	AWU_4S,       /**< 4 seconds */
	AWU_8S,       /**< 8 seconds */
	AWU_16S       /**< 16 seconds */
} awu_tick_t;

/**
 * @brief Start an AUX command immediately, without an initial AWU delay.
 *
 * This is a sentinel value for start_aux_cmd(); it must not be passed to
 * AWU_set_delay() because it is not a hardware AWU interval.
 */
#define AWU_IMMEDIATELY ((awu_tick_t)0xFFU)

//extern volatile uint8_t awu_event; /**< AWU event flag, set in ISR */
extern volatile uint8_t is_sleep_mode; /**< Non-zero when MCU is in STANDBY */

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
 * @param time_aux Requested delay interval.
 * @return true if the interval was set successfully.
 */
bool AWU_set_delay(awu_tick_t time_aux);

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
 * @brief Configure AWU and enter STANDBY immediately.
 */
void AWU_Sleep_Now(void);

#endif /* AWU_H */
