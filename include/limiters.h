/**
 * @file limiters.h
 * @brief Limiter module (thermal + config)
 *
 * Provides system initialisation, periodic limiter scanning,
 * and access to the shared thermal limiter config/state.
 */

#ifndef LIMITERS_H
#define LIMITERS_H

#include <stdbool.h>
#include "thermal_limiter.h"

/** @brief ADC value in termo_table at 50 °C (table index 70). */
#define TERMO_50C_ADC    722u
/** @brief Default t_calibr value stored in EE.usr.t_calibr. */
#define TCALIBR_DEFAULT  6u
/**
 * @brief Base temperature ADC threshold for thermal limiting.
 *
 * Computed as TERMO_50C_ADC minus default t_calibr offset.
 * Used in thermal limiter calculations.
 */
#define BASE_TEMPERATURE  (TERMO_50C_ADC - TCALIBR_DEFAULT)

/* ===== Public API ===== */

/**
 * @brief System initialisation (clock, GPIO, peripherals).
 */
void sys_init(void);

/**
 * @brief Scan thermal and electrical limiters during the fast thermal phase.
 */
void scan_limiters(void);

/**
 * @brief Apply the sensor-fault failsafe after an ADC acquisition failure.
 */
void scan_limiters_fault(void);

/** @brief Latched after THERMAL_FAULT_CONFIRM consecutive rail/timeouts. */
extern bool thermal_fault;

/**
 * @brief Check whether the last temperature acquisition is unsafe to use.
 * @return true after a confirmed sensor rail or ADC acquisition fault.
 */
#define thermal_fault_active()  (thermal_fault)

/**
 * @brief Reset limiter state to defaults.
 */
void reset_limiters(void);

extern uint16_t limiter;                          /**< Current limiter value (PWM ceiling) */
extern ThermalLimiterState_t  state;              /**< Thermal limiter runtime state */
extern const ThermalLimiterConfig_t config;        /**< Thermal limiter configuration (flash) */

#endif /* LIMITERS_H */
