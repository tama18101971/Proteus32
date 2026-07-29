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
    .kp_left_shift        = 5,
    .ki_decimation_shift  = 7,
    .ki_left_shift        = 0,
    .t_anticipate_offset  = 30,
    .filter_shift         = 1,
    .max_drop_rate        = 3,
    .max_recovery_rate    = 1,
    .overheat_leak_decimation_shift = 6,
    .max_limiter          = 255
};

/** @brief Current limiter value (PWM ceiling). */
uint16_t limiter = 0;

/** @brief Initialise thermal limiter state. */
void sys_init(void)
{
    ThermalLimiter_Init(&state);
}

/** @brief Scan thermal limiter (call at 30 Hz). */
void scan_limiters(void)
{
#define MIN(a, b) ((a < b) ? (a) : (b))

    static uint8_t old_correction;

    if (driver_mode < MODE_ON)
        return;

    uint16_t exact_adc = get_exact_temp_adc();
    uint16_t t_set = BASE_TEMPERATURE + (uint16_t)EE.usr.t_calibr;
    limiter = ThermalLimiter_Update(&config, &state, exact_adc, t_set, step_user);

    if (old_correction != limiter)
    {
        uint16_t up_level = MAX_STEP - limiter;
        set_current_ll(MIN(up_level, step_user), SS_NO);

        if ((EE.uif.ui_index > 1) && (mode_current == CMODE_TURBO))
        {
            if (up_level < get_stepi(CMODE_MAX))
            {
                mode_current = CMODE_MAX;
                ps.use_prev = false;
            }
        }
        old_correction = limiter;
    }
}

/** @brief Reset all limiters to initial state. */
void reset_limiters(void)
{
    ThermalLimiter_Init(&state);
    limiter = 0;
}