#ifndef ADC_H_
#define ADC_H_

/**
 * @file adc.h
 * @brief ADC driver for NTC temperature and supply voltage measurement
 *
 * ADC1 is used for:
 *   - NTC thermistor temperature measurement (channel 3, PD2)
 *   - Supply voltage measurement via Vrefint (channel 8)
 *
 * Functions:
 *   adc_init()  — full ADC initialisation with calibration
 *   adc_read()  — single measurement (NTC + Vrefint), outputs results
 *   adc_stop()  — fully stop ADC (clock off, GPIO Hi-Z)
 *
 * EMA filter (Exponential Moving Average):
 *   ema_init()  — initialise filter with seed value
 *   ema_update() — update filter with new raw value
 *
 * Temperature table: -20..+80°C, 1°C step
 */

#include <stdint.h>

#define TEMPERATURE_TABLE_START  (-200)
#define TEMPERATURE_TABLE_STEP   (10)

/**
 * @brief Perform one ADC measurement cycle (NTC + Vrefint).
 *
 * Reads NTC temperature channel and Vrefint, updates global variables:
 * vbat_mv, temperature_c, raw_temperature_adc.
 */
void adc_read(void);

/**
 * @brief Initialise ADC1 with calibration.
 *
 * Configures ADC for NTC (channel 3, PD2) and Vrefint (channel 8).
 * Enables ADC clock, runs self-calibration.
 */
void adc_init(void);

/**
 * @brief Fully stop ADC and release resources.
 *
 * Disables ADC clock, sets GPIO pins to Hi-Z for power saving.
 */
void adc_stop(void);

/**
 * @brief Convert raw ADC value to temperature (x0.1°C).
 * @param adcValue Raw 10-bit ADC reading from NTC channel.
 * @return Temperature in deci-degrees Celsius (e.g. 250 = 25.0°C).
 */
int16_t calc_temperature(uint16_t adcValue);

/* EMA filter for Vrefint ADC (supply voltage Vdd measurement).
 * shift=4: alpha=1/16, lag ~16 samples.
 * shift=3: alpha=1/8,  lag ~8 samples.
 * Higher shift = smoother but slower response. */
#define EMA_SHIFT 4

/**
 * @brief Initialise EMA filter with a seed value.
 * @param seed Initial filter value.
 */
void     ema_init(uint16_t seed);

/**
 * @brief Update EMA filter with new raw sample.
 * @param raw Raw ADC value.
 * @return Filtered value.
 */
uint16_t ema_update(uint16_t raw);

/** @brief Battery voltage in millivolts (updated by adc_read). */
extern uint16_t vbat_mv;

/** @brief Temperature in °C (updated by adc_read). */
extern int16_t temperature_c;

/** @brief Raw temperature ADC value (updated by adc_read). */
extern uint16_t raw_temperature_adc;

/**
 * @brief Get number of battery indication flashes.
 * @return Number of flashes (0..6).
 */
uint8_t bat_get_flashes(void);

/* ============ Thermal calibration constants ============ */

#define REF_TEMP_ADC    542    /**< Expected ADC at 25°C — termo_table[45] */
#define REF_VOLTAGE_MV  4300   /**< Expected Vbat during calibration (mV) */

#define VC_CLIP_MAX     127    /**< Max vc value (int8_t, ±127) */
#define VC_CLIP_MIN     -127
#define TC_CLIP_MAX     31     /**< Max tc value */
#define TC_CLIP_MIN     -31

#define ADC_MIN_VAL     0      /**< 10-bit ADC range minimum */
#define ADC_MAX_VAL     1023   /**< 10-bit ADC range maximum */

/**
 * @brief Get temperature ADC corrected by calibration offset.
 * @return raw_temperature_adc - tc, clamped to 0..1023.
 */
uint16_t get_exact_temp_adc(void);

/* ============ Battery protection ============ */

/**
 * @brief Main battery protection loop (call at 30 Hz after adc_read()).
 *
 * Implements UI1 ramping and UI2 discrete mode voltage protection.
 * Updates volt_max_mode, volt_stop_mode, down_count, volt_step_ratchet.
 */
void scan_voltage(void);

/** @brief Dynamic UI2 mode ceiling (updated in scan_voltage). */
extern uint8_t volt_max_mode;

/** @brief Ratchet memory — historical minimum of volt_max_mode. */
extern uint8_t volt_stop_mode;

/** @brief UI1 step-down counter (reset in on_driver). */
extern uint8_t down_count;

/** @brief UI1 sag ratchet: monotonically decreasing step ceiling by voltage. */
extern uint16_t volt_step_ratchet;

/**
 * @brief Apply the UI1 sag ratchet and return the current ceiling.
 *
 * Lowers volt_step_ratchet to the Vbat-based tier when it is lower,
 * then returns it. This function has a side effect.
 */
uint16_t ratchet_max_step(void);

/**
 * @brief Clear the historical UI1 voltage ceiling after a confirmed recovery.
 *
 * Call only after a fresh voltage measurement is above the configured
 * recovery threshold; this does not disable the low-voltage step-down logic.
 */
void reset_max_step(void);

#endif /* ADC_H_ */