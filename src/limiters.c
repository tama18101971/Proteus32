/**
 * @file limiters.c
 * @brief Thermal and electrical limiter coordinator
 *
 * Implements sys_init(), scan_limiters() and reset_limiters().
 * Thermal limiter configuration and runtime state are defined here
 * (see thermal_limiter.h/c for the PI controller implementation).
 */

#include "adc.h"
#include "ui.h"
#include "limiters.h"
#include "app_data.h"
#include "proteus_config.h"

/** @brief Thermal limiter runtime state (RAM). */
ThermalLimiterState_t  state;
/** @brief Thermal limiter configuration (flash). */
const ThermalLimiterConfig_t config = {
    .kp_left_shift        = 3,
    .ki_decimation_shift  = 7,
    .ki_left_shift        = 0,
    .t_anticipate_offset  = 30,
    .filter_shift         = 3,
    .max_drop_rate        = 3,
    .max_recovery_rate    = 1,
    .overheat_leak_decimation_shift = 6,
    .max_limiter          = 255
};

/** @brief Current limiter amount subtracted from the user brightness. */
uint16_t limiter = 0;

/** @brief Last applied limiter amount (gate: re-target only on change). */
static uint16_t last_limiter = 0;

/** @brief Latched after THERMAL_FAULT_CONFIRM consecutive rail/timeouts. */
bool thermal_fault;

/** Consecutive rail/timeout samples required before the LED is forced off. */
#define THERMAL_FAULT_CONFIRM  3u

/** @brief Consecutive failed temperature acquisitions (rail or timeout). */
static uint8_t fault_streak;

/**
 * @brief Apply the current thermal reduction to the physical LED output.
 *
 * The requested brightness remains in step_user.  Only the physical step is
 * reduced.  An active user ramp is left running: ss_scan() clamps it to
 * step_user - limiter every tick, so this path must not start a second
 * competing soft-start.
 */
static void apply_limiter_output(void)
{
    uint16_t limited_step = (step_user > limiter) ?
                            (uint16_t)(step_user - limiter) : 0u;

    if (!ss.enable)
        set_current_ll(limited_step, SS_NO);
}

/**
 * @brief Leave UI2 turbo when thermal limiting is below the max-mode level.
 * @param limited_step Current physical brightness after thermal reduction.
 */
static void check_turbo_thermal_limit(uint16_t limited_step)
{
    if ((ui_table_idx == 2U || ui_table_idx == 3U) &&
        (mode_current == CMODE_TURBO) &&
        (limited_step < get_stepi(CMODE_MAX)))
    {
        mode_current = CMODE_MAX;
        ps.use_prev = false;
    }
}

/**
 * @brief Run the thermal controller for one temperature sample.
 * @param exact_adc Corrected temperature ADC value (THERMAL_ADC_MIN when
 *                  the sample is unusable — the controller then holds the
 *                  previous limiter and does not contaminate its filter).
 */
static void update_limiters(uint16_t exact_adc)
{
    uint16_t t_set = BASE_TEMPERATURE + (uint16_t)EE.usr.t_calibr;
    limiter = ThermalLimiter_Update(&config, &state, exact_adc, t_set, step_user);
    uint16_t limited_step = (step_user > limiter) ?
                            (uint16_t)(step_user - limiter) : 0u;

    if (limiter != last_limiter)
    {
        last_limiter = limiter;
        apply_limiter_output();
    }

    check_turbo_thermal_limit(limited_step);
}

/** @brief Initialise thermal limiter state. */
void sys_init(void)
{
    ThermalLimiter_Init(&state);
}

/** @brief Scan thermal limiter once for a fresh fast-phase ADC sample. */
void scan_limiters(void)
{
    if ((raw_temperature_adc == ADC_MIN_VAL) ||
        (raw_temperature_adc >= ADC_MAX_VAL))
    {
        scan_limiters_fault();
        return;
    }

    fault_streak = 0u;
    thermal_fault = false;
    if (driver_mode == MODE_ON)
        update_limiters(get_exact_temp_adc());
}

/**
 * @brief Count a failed acquisition; shut the LED down only after confirmation.
 *
 * A single rail/timeout must not flash the output.  Until the streak
 * reaches THERMAL_FAULT_CONFIRM the last safe limiter is left in place.
 */
void scan_limiters_fault(void)
{
    if (fault_streak < 255u)
        fault_streak++;
    if (fault_streak < THERMAL_FAULT_CONFIRM)
        return;

    thermal_fault = true;
    if (driver_mode != MODE_ON)
        return;

    state.flags |= THERMAL_FLAG_ERROR;
    limiter = step_user;
    state.prev_limiter = (int16_t)step_user;
    last_limiter = limiter;
    set_current_ll(0, SS_NO);
}

/** @brief Reset all limiters to initial state. */
void reset_limiters(void)
{
    ThermalLimiter_Init(&state);
    limiter = 0;
    last_limiter = 0;
    thermal_fault = false;
    fault_streak = 0u;
}
