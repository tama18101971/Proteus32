/**
 * @file    thermal_limiter.h
 * @brief   Zero-multiplication Thermal Power Limiter with continuous PI
 *          regulation and equilibrium hold band for high-power LED,
 *          targeting the WCH CH32V003 (RV32EC, 16KB Flash / 2KB RAM).
 *
 * @details
 *  Architecture: Continuous PI Controller with Equilibrium Hold Band
 *  ------------------------------------------------------------------------
 *  Provides continuous, oscillation-free thermal regulation:
 *    - Proportional term (P): instantaneous braking on temperature spikes
 *      when t_cur >= t_set.
 *    - Integral term (I): fixed-point accumulator (6 fractional bits)
 *      representing the steady-state limiter required to balance heat
 *      generation and dissipation at t_set.
 *    - Equilibrium Hold Band (t_set - t_anticipate_offset <= t_cur < t_set):
 *      When temperature is within the target band, the integral accumulator
 *      is held constant (delta_I = 0). This eliminates hunting/oscillation
 *      and holds the maximum sustainable brightness.
 *    - Smooth Recovery (t_cur < t_set - t_anticipate_offset):
 *      When genuinely cool, the integral gently decreases proportionally
 *      to the negative error, smoothly restoring brightness without surges.
 *    - Summing (P + I): linear combination clamped to [0, user_pwm].
 *    - Slew Rate Limiter: asymmetric drop/recovery rates prevent any
 *      visible flicker or sudden stepping to human eyes.
 */

#ifndef THERMAL_LIMITER_H
#define THERMAL_LIMITER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Compile-time constants                                                   */
/* ------------------------------------------------------------------------ */

/** 10-bit ADC domain limits for `t_raw`. */
#define THERMAL_ADC_MIN            0u
#define THERMAL_ADC_MAX            1023u

/** 9-bit user PWM / Limiter domain (0..512 inclusive). */
#define THERMAL_PWM_MAX            512u

/** Fixed-point fractional bits for the integral accumulator (64 sub-steps). */
#define THERMAL_FP_SHIFT           6u

/** Fixed-point fractional bits for the temperature filter (32 sub-steps). */
#define THERMAL_TEMP_FP_SHIFT      5u

/** Flag bits packed into ::ThermalLimiterState_t::flags. */
#define THERMAL_FLAG_INITIALIZED   0x01u /**< EMA filter seeded. */
#define THERMAL_FLAG_ERROR         0x10u /**< Sensor/ADC hardware fault. */

/* ------------------------------------------------------------------------ */
/* Configuration (read-only, flash-resident)                                */
/* ------------------------------------------------------------------------ */

/**
 * @brief Tunable, shift-based parameters of the thermal limiter.
 */
typedef struct {
    uint8_t  kp_left_shift;        /**< Proportional gain: P = (t_cur - t_set) << kp_left_shift */
    uint8_t  ki_decimation_shift;  /**< Integral heat scale: delta_I = max(1, error >> ki_decimation_shift) */
    uint8_t  ki_recov_shift;       /**< Integral cool scale: delta_I = -max(1, cool_error >> ki_recov_shift) */
    uint16_t t_anticipate_offset;  /**< Hold band (ADC counts): t_threshold = t_set - offset */
    uint8_t  filter_shift;         /**< EMA filter shift: filtered += (raw - filtered) >> filter_shift */
    uint16_t max_drop_rate;        /**< Max limiter increase per tick (brightness cut speed) */
    uint16_t max_recovery_rate;    /**< Max limiter decrease per tick (brightness restore speed) */
    uint16_t max_limiter;          /**< Hard ceiling on the Limiter output (0..512) */
} ThermalLimiterConfig_t;

/* ------------------------------------------------------------------------ */
/* Runtime state (RAM-resident)                                             */
/* ------------------------------------------------------------------------ */

/**
 * @brief Persistent state of a single thermal limiter instance.
 */
typedef struct {
    int16_t  filtered_temp;     /**< EMA-filtered rounded ADC reading (t_cur) */
    int16_t  filtered_temp_fp;  /**< Fixed-point EMA temperature accumulator (scale x32) */
    int16_t  integral;          /**< Integer integral limiter value (0..512) */
    int16_t  sub_integral;      /**< Fractional sub-step accumulator (0..63) */
    int16_t  prev_limiter;      /**< Previous tick's post-slew Limiter value (0..512) */
    uint8_t  flags;             /**< Status flags (INITIALIZED, ERROR) */
} ThermalLimiterState_t;

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

/**
 * @brief Reset a limiter instance to its power-on-safe state.
 * @param state  State instance to initialize.
 */
void ThermalLimiter_Init(ThermalLimiterState_t *state);

/**
 * @brief Run one iteration of the thermal power limiter control loop.
 *
 * @param config    Controller configuration/tuning.
 * @param state     Persistent controller state (RAM).
 * @param t_raw     Raw 10-bit ADC reading from NTC divider (0..1023).
 * @param t_set     10-bit ADC target set-point (0..1023).
 * @param user_pwm  9-bit user-requested brightness (0..512).
 * @return          Limiter amount to subtract from user_pwm (0..user_pwm).
 */
uint16_t ThermalLimiter_Update(const ThermalLimiterConfig_t *config,
                                ThermalLimiterState_t *state,
                                uint16_t t_raw,
                                uint16_t t_set,
                                uint16_t user_pwm);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_LIMITER_H */
