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

/** @brief Current limiter amount subtracted from the user brightness. */
uint16_t limiter = 0;

/** @brief Limiter value used by the last physical output update. */
static uint16_t old_limiter = 0;
/** @brief Latched temperature acquisition fault until a valid sample arrives. */
static bool thermal_fault;

/**
 * @brief Apply the current thermal reduction to the physical LED output.
 *
 * The requested brightness remains in step_user.  Only the physical step is
 * reduced, so a later fresh temperature sample can recover brightness without
 * losing the user's requested level.
 */
static void apply_limiter_output(void)
{
    uint16_t limited_step = (step_user > limiter) ?
                            (uint16_t)(step_user - limiter) : 0u;

    /* Thermal output is authoritative.  A pending user soft-start must not
     * write its old, unrestricted target after this update. */
    ss_cancel();
    set_current_ll(limited_step, SS_NO);

    if ((EE.uif.ui_index > 1) && (mode_current == CMODE_TURBO) &&
        (limited_step < get_stepi(CMODE_MAX)))
    {
        mode_current = CMODE_MAX;
        ps.use_prev = false;
    }
}

/**
 * @brief Run the thermal controller for one temperature sample.
 * @param exact_adc Corrected temperature ADC value, or zero for failsafe.
 */
static void update_limiters(uint16_t exact_adc)
{
    uint16_t t_set = BASE_TEMPERATURE + (uint16_t)EE.usr.t_calibr;
    limiter = ThermalLimiter_Update(&config, &state, exact_adc, t_set, step_user);

    if (old_limiter != limiter)
    {
        apply_limiter_output();
        old_limiter = limiter;
    }
}

/** @brief Initialise thermal limiter state. */
void sys_init(void)
{
    ThermalLimiter_Init(&state);
}

/** @brief Scan thermal limiter once for a fresh fast-phase ADC sample. */
void scan_limiters(void)
{
    bool rail_fault = (raw_temperature_adc == ADC_MIN_VAL) ||
                      (raw_temperature_adc >= ADC_MAX_VAL);

    if (driver_mode != MODE_ON)
    {
        thermal_fault = rail_fault;
        if (rail_fault)
            old_limiter = UINT16_MAX;
        return;
    }

    thermal_fault = rail_fault;
    if (rail_fault)
    {
        update_limiters(THERMAL_ADC_MIN);
        return;
    }

    update_limiters(get_exact_temp_adc());
}

/**
 * @brief Force the thermal limiter into its sensor-fault failsafe.
 *
 * A conversion timeout is not a temperature reading.  Passing the ADC
 * minimum directly to the unchanged limiter makes the physical output safe
 * without contaminating the filtered temperature or integral state.
 */
void scan_limiters_fault(void)
{
    thermal_fault = true;
    if (driver_mode != MODE_ON)
    {
        old_limiter = UINT16_MAX;
        return;
    }

    update_limiters(THERMAL_ADC_MIN);
}

/**
 * @brief Check whether the last temperature acquisition is unsafe to use.
 * @return true after a sensor rail or ADC acquisition fault until a valid
 *         temperature sample clears it.
 */
bool thermal_fault_active(void)
{
    return thermal_fault;
}

/** @brief Reset all limiters to initial state. */
void reset_limiters(void)
{
    ThermalLimiter_Init(&state);
    limiter = 0;
    old_limiter = 0;
}
