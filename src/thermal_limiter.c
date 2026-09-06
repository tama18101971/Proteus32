/**
 * @file    thermal_limiter.c
 * @brief   Zero-multiplication continuous PI Thermal Power Limiter with
 *          equilibrium hold band.
 */

#include "thermal_limiter.h"

/**
 * @brief Reset a limiter instance to its power-on-safe state.
 * @param state  State instance to initialize.
 */
void ThermalLimiter_Init(ThermalLimiterState_t *state)
{
    state->filtered_temp    = 0;
    state->filtered_temp_fp = 0;
    state->integral         = 0;
    state->sub_integral     = 0;
    state->prev_limiter     = 0;
    state->flags            = 0u;
}

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
                                uint16_t user_pwm)
{
    /* a) Failsafe: check sensor rail limits */
    if (t_raw == THERMAL_ADC_MIN || t_raw >= THERMAL_ADC_MAX) {
        state->flags |= THERMAL_FLAG_ERROR;
        return (uint16_t)state->prev_limiter;
    }
    state->flags &= (uint8_t)~THERMAL_FLAG_ERROR;

    /* b) Seed filter on first run */
    if (!(state->flags & THERMAL_FLAG_INITIALIZED)) {
        state->filtered_temp    = (int16_t)t_raw;
        state->filtered_temp_fp = (int16_t)((int16_t)t_raw << THERMAL_TEMP_FP_SHIFT);
        state->flags           |= THERMAL_FLAG_INITIALIZED;
    }

    /* c) Fixed-Point EMA Low-Pass Filter (eliminates integer truncation bias) */
    int16_t diff = (int16_t)(((int16_t)t_raw << THERMAL_TEMP_FP_SHIFT) - state->filtered_temp_fp);
    state->filtered_temp_fp = (int16_t)(state->filtered_temp_fp + (diff >> config->filter_shift));
    state->filtered_temp    = (int16_t)((state->filtered_temp_fp + (1 << (THERMAL_TEMP_FP_SHIFT - 1u))) >> THERMAL_TEMP_FP_SHIFT);

    int16_t t_cur_fp       = state->filtered_temp_fp;
    int16_t t_set_fp       = (int16_t)((int16_t)t_set << THERMAL_TEMP_FP_SHIFT);
    int16_t t_threshold_fp = (int16_t)(((int16_t)t_set - (int16_t)config->t_anticipate_offset) << THERMAL_TEMP_FP_SHIFT);

    /* d) Continuous PI terms with Equilibrium Hold Band */
    int16_t p_term = 0;
    int16_t delta_i = 0;

    if (t_cur_fp >= t_set_fp) {
        /* Overheat zone: proportional braking + integral accumulation.
         * P-терм активен только выше зоны нечувствительности p_deadband
         * (в кодах АЦП): шум ±1..2 LSB возле уставки больше не меняет
         * яркость на полные шаги с частотой опроса — мерцание подавлено,
         * установившийся режим регулируется исключительно интегратором
         * (субшаг 1/64, монотонный, ~2 с на шаг). Интегратор учитывает
         * ПОЛНУЮ ошибку (без вычета deadband), чтобы точно удерживать
         * t_set, а не t_set + deadband. */
        int16_t error_fp = (int16_t)(t_cur_fp - t_set_fp);
        int16_t p_deadband_fp = (int16_t)((int16_t)config->p_deadband << THERMAL_TEMP_FP_SHIFT);
        if (error_fp > p_deadband_fp) {
            int16_t active_error_fp = (int16_t)(error_fp - p_deadband_fp);
            int32_t p_wide = ((int32_t)active_error_fp << config->kp_left_shift) >> THERMAL_TEMP_FP_SHIFT;
            if (p_wide > (int32_t)user_pwm) {
                p_wide = (int32_t)user_pwm;
            }
            p_term = (int16_t)p_wide;
        }
        int16_t error = (int16_t)(error_fp >> THERMAL_TEMP_FP_SHIFT);
        delta_i = (int16_t)(error >> config->ki_decimation_shift);
        if (delta_i == 0 && error_fp > 0) {
            delta_i = 1;
        }
    } else if (t_cur_fp >= t_threshold_fp) {
        /* Equilibrium hold band: zero drift, holds steady-state output */
        p_term = 0;
        delta_i = 0;
    } else {
        /* Cool / Recovery zone: gentle proportional decay */
        p_term = 0;
        int16_t cool_error_fp = (int16_t)(t_threshold_fp - t_cur_fp);
        int16_t cool_error = (int16_t)(cool_error_fp >> THERMAL_TEMP_FP_SHIFT);
        delta_i = (int16_t)(-(cool_error >> config->ki_recov_shift));
        if (delta_i == 0 && cool_error_fp > 0) {
            delta_i = -1;
        }
    }

    /* e) Fixed-point integral update with anti-windup clamping */
    int32_t total_fp = ((int32_t)state->integral << THERMAL_FP_SHIFT) + (int32_t)state->sub_integral;
    total_fp += delta_i;

    int32_t max_fp = (int32_t)user_pwm << THERMAL_FP_SHIFT;
    if (total_fp > max_fp) {
        total_fp = max_fp;
    } else if (total_fp < 0) {
        total_fp = 0;
    }

    state->integral     = (int16_t)(total_fp >> THERMAL_FP_SHIFT);
    state->sub_integral = (int16_t)(total_fp & ((1u << THERMAL_FP_SHIFT) - 1u));

    /* f) Summing P + I and Slew Rate Filter for smooth transitions */
    int16_t raw_limiter  = (int16_t)(state->integral + p_term);
    int16_t last_limiter = state->prev_limiter;
    int16_t delta        = (int16_t)(raw_limiter - last_limiter);
    int16_t next_limiter;

    if (delta > (int16_t)config->max_drop_rate) {
        next_limiter = (int16_t)(last_limiter + (int16_t)config->max_drop_rate);
    } else if (delta < -(int16_t)config->max_recovery_rate) {
        next_limiter = (int16_t)(last_limiter - (int16_t)config->max_recovery_rate);
    } else {
        next_limiter = raw_limiter;
    }

    if (next_limiter < 0) {
        next_limiter = 0;
    } else if (next_limiter > (int16_t)user_pwm) {
        next_limiter = (int16_t)user_pwm;
    }

    if (next_limiter > (int16_t)config->max_limiter) {
        next_limiter = (int16_t)config->max_limiter;
    }

    state->prev_limiter = next_limiter;
    return (uint16_t)next_limiter;
}
