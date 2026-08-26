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

/** @brief Default t_calibr value stored in EE.usr.t_calibr (5 clicks = 50 °C). */
#define TCALIBR_DEFAULT  5u

/* ===== Public API ===== */

/**
 * @brief Get the thermal limiter setpoint (t_set) in ADC codes from EE.usr.t_calibr.
 * @return Target ADC code for thermal limiter.
 */
uint16_t get_thermal_setpoint(void);

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
