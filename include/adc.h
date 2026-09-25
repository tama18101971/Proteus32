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
 *   adc_read()  — single verified measurement (NTC + Vrefint), outputs results
 *   adc_stop()  — fully stop ADC (clock off, GPIO Hi-Z)
 *
 * EMA filter (Exponential Moving Average):
 *   ema_update() — update filter with new raw value (seeds itself on the first
 *                  sample, so no explicit init is needed)
 *
 * Temperature table: -20..+80°C, 1°C step
 */

#include <stdbool.h>
#include <stdint.h>
#include "proteus_config.h"

#define TEMPERATURE_TABLE_START  (-200)
#define TEMPERATURE_TABLE_STEP   (10)

/**
 * @brief Perform one verified ADC measurement cycle (NTC + Vrefint).
 *
 * Reads NTC temperature channel and Vrefint. Global values are updated only
 * when both conversions complete and Vrefint is usable. Vrefint codes above
 * 512 (vbat below 2.4 V) are deliberately committed as valid deep-discharge
 * samples so the four-sample undervoltage latch can fire (B4); only an
 * implausibly high voltage (code below 240) is rejected.
 * @return true when a fresh pair was committed, false on acquisition failure.
 */
bool adc_read(void);

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
 * @brief Update EMA filter with new raw sample.
 * @param raw Raw ADC value.
 * @return Filtered value.
 */
uint16_t ema_update(uint16_t raw);

/* ----------------------------------------------------------------------------
 * Напряжение считается целиком в кодах Vrefint:  vbat_mv = floor(1228800/adc).
 * Vrefint монотонно убывает при росте напряжения, поэтому все пороги защиты
 * переводятся в коды времени компиляции и сравниваются без делений.
 *   MV_GE(mv) = floor(1228800/mv):  vbat >= mV  ⟺  adc <= MV_GE(mV)
 *   MV_LT(mv) = floor(1228800/mv)+1: vbat <  mV ⟺  adc >= MV_LT(mV)
 * Калибровочный сдвиг vc (Option Bytes, int8_t) хранит не более ±22 кодов
 * (окно ±0.29 В вокруг REF_VOLTAGE_MV, см. VC_CLIP_*) и входит в
 * сравнение аддитивно: эффективный код e = adc - vc.
 * ------------------------------------------------------------------------- */

#define VREF_NUMERATOR (1200u * 1024u)          /* 1228800: мВ * 2^10 */
#define MV_GE(mv)      (VREF_NUMERATOR / (uint32_t)(mv))
#define MV_LT(mv)      (MV_GE(mv) + 1u)

/**
 * @brief Измеренный код Vrefint после EMA-фильтра (обновляется в adc_read).
 * Основная физическая величина ветки контроля напряжения в кодах.
 */
extern uint16_t vrefint_adc;

/**
 * @brief Последний проверенный СЫРОЙ код Vrefint (до EMA, обновляется в adc_read).
 * Используется обоими необратимыми порогами scan_voltage() — аварийным
 * backstop (adc_eff_raw_vcode + UV_DEEP_*) и отсечением 2.70 В (swt_cut[0]) —
 * а также гардами строба/маяка на AWU-пробуждениях и калибровкой vc
 * (усреднение серии свежих пар): EMA после скачка стенда отстаёт от raw.
 */
extern uint16_t vrefint_raw_adc;

/**
 * @brief Признак валидной свежей выборки напряжения.
 * false — после таймаута/сбоя АЦП (main); adc_eff_vcode()/adc_eff_raw_vcode()
 * отдают sentinel («критически низкое»), но scan_voltage() на такой выборке
 * не пересчитывает потолок яркости и не снижает режим — сохраняется
 * последнее достоверное значение; отсечение 2.70 В (raw) и аварийный
 * backstop 2.50 В (raw) также выполняются только на валидной выборке.
 */
extern bool voltage_ok;

/**
 * @brief Пересчёт кода Vrefint в милливольты (точно, без libgcc).
 * @param vrefint_adc Код Vrefint. Сырая выборка обычно 240..512; при
 *                    глубоком разряде (< 2.4 В, B4) код доходит до 1023.
 *                    Диагностический вывод дополнительно сдвигает код на vc.
 * @return Напряжение в мВ (floor(1228800/adc)). 0 при adc==0.
 * Используется только для отладочного вывода — горячий путь в кодах.
 */
/* The implementations are compiled only for diagnostic builds.  Keep the
 * declarations unconditional because this header may be included before
 * proteus_config.h defines UART_DEBUG/PRINT_ADC. */
uint16_t vbat_to_mv(uint16_t vrefint_adc);

/**
 * @brief Get the calibrated battery voltage for diagnostic output.
 *
 * Applies the Option Bytes code offset to the current filtered Vrefint code
 * and returns zero when the current voltage sample is not valid. This helper
 * is intentionally kept out of the battery-protection hot path.
 * @return Calibrated battery voltage in mV, or 0 without a valid sample.
 */
uint16_t vbat_to_mv_calibrated(void);

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
#define REF_VOLTAGE_MV  4200   /**< Expected Vbat during calibration (mV) */

/* Код Vrefint, соответствующий опорному напряжению калибровки (4200 мВ).
 * Калибровка vc = vrefint_adc - REF_VREF_CODE (коды, до +22/−18), сохранённая
 * в Option Bytes. */
#define REF_VREF_CODE  (MV_GE(REF_VOLTAGE_MV))

/* Допустимый диапазон коррекции vc ограничен ±0.29 В от REF_VOLTAGE_MV:
 * калибровка проходит только при стендовом напряжении 3.91..4.49 В.
 * Границы считаются в кодах Vrefint из mV-окна (направление обратное из-за
 * зависимости vbat = 1228800/adc): VC_CLIP_MAX отвечает коду при 4200−290 мВ,
 * VC_CLIP_MIN — коду при 4200+290 мВ.  Выход за окно отвергает calibration(). */
#define VC_VOLTAGE_WINDOW_MV  290u  /**< Допустимое отклонение стенда (mV), ±0.29 В */
#define VC_CLIP_MAX           (int)(MV_GE(REF_VOLTAGE_MV - VC_VOLTAGE_WINDOW_MV) - REF_VREF_CODE)
#define VC_CLIP_MIN           (int)(MV_LT(REF_VOLTAGE_MV + VC_VOLTAGE_WINDOW_MV) - REF_VREF_CODE)
#define TC_CLIP_MAX     44     /**< Max tc value (±5 °C at ~8.8 codes/°C) */
#define TC_CLIP_MIN     -44

#define ADC_MIN_VAL     0      /**< 10-bit ADC range minimum */
#define ADC_MAX_VAL     1023   /**< 10-bit ADC range maximum */

/**
 * @brief Get temperature ADC corrected by calibration offset.
 * @return raw_temperature_adc - tc, clamped to 0..1023.
 */
uint16_t get_exact_temp_adc(void);

/* ============ Battery protection ============ */

/* Количество последовательных подтверждений низкого напряжения до срабатывания
 * защиты. Сравнение строгое (++count > UNDERVOLT_CONFIRM_COUNT), поэтому
 * защёлка включается на 4-м подряд попадании ниже порога; одиночный шум
 * или невалидная выборка сбрасывают счётчик. Используется scan_voltage()
 * (voff_count — серия снижения режима по каскаду для всех UI;
 * voff_count_off — серия отсечения 2.70 В на raw-канале)
 * и strobe_voltage_low() (low_count). */
#define UNDERVOLT_CONFIRM_COUNT  3u

/* Аварийный backstop глубокого разряда (scan_voltage): работает на любой
 * яркости, пока постепенный спуск ещё не дошёл до минимального режима, а АКБ
 * проваливается быстрее каскада. Порог — на raw-канале: необратимый
 * аварийный порог не должен зависеть от параметров EMA и частоты опроса
 * (в термо-сне AWU_4S лаг EMA до ~64 с). 2.50 В — критическое напряжение
 * разряда Li-ion по даташиту; штатное отсечение 2.70 В на минимальном режиме
 * (raw-канал, swt_cut[0]) срабатывает раньше во всех достижимых точках.
 * Сравнение строгое (++count > UV_DEEP_CONFIRM); невалидная выборка и
 * отскок напряжения после шага каскада сбрасывают серию. */
#define UV_DEEP_MV       2500u  /**< Backstop: vbat < 2.50 В (raw) */
#define UV_DEEP_VCODE    (MV_LT(UV_DEEP_MV)) /**< Код порога backstop (raw - vc) */
#define UV_DEEP_CONFIRM  8u     /**< Подтверждений подряд до down_forever() */

/**
 * @brief Калиброванный код Vrefint с учётом vc и валидности выборки (после EMA).
 * Единый канал сглаженного регулирования: потолок яркости, каскад режимов,
 * предупреждения и индикация. Оба необратимых порога (отсечение 2.70 В
 * swt_cut[0] и аварийный backstop 2.50 В) — на raw-канале,
 * см. adc_eff_raw_vcode().
 * @return (vrefint_adc - vc) при валидной выборке, иначе код-сентинел
 *         «критически низкого напряжения».
 */
int32_t adc_eff_vcode(void);

/**
 * @brief Калиброванный СЫРОЙ код Vrefint (без EMA) для необратимых порогов.
 * Используется отсечением 2.70 В (swt_cut[0]) на минимальном режиме и
 * аварийным backstop UV_DEEP_VCODE (2.50 В) в scan_voltage(), а также
 * гардами строба/маяка на AWU-пробуждениях. Необратимые пороги не должны
 * зависеть от параметров фильтра и частоты опроса: на 30 Гц EMA отстаёт
 * от raw до 8 кодов (~45 мВ) при медленном разряде.
 * @return (raw vrefint - vc) при валидной выборке, иначе код-сентинел
 *         «критически низкого напряжения».
 */
int32_t adc_eff_raw_vcode(void);

/**
 * @brief Обновить флаг низкого заряда маяка по гистерезису 3.10 / 3.20 В.
 *
 * Ниже 3.10 В — двойные вспышки, выше 3.20 В — одиночные; между порогами
 * состояние не меняется. Только на валидной выборке (voltage_ok).
 * @return true если beacon_low_voltage изменился.
 */
bool beacon_low_voltage_update(void);

/**
 * @brief Main battery protection loop (call after a successful adc_read()).
 *
 * Unified confirmed mode cascade (mode_current) for ALL UIs; UI1/tactical
 * clamp step_user to get_stepi(mode_current). Updates volt_stop_mode.
 */
void scan_voltage(void);

/**
 * @brief Voltage latch — monotonic latch, only decreases during a sag
 * (подтверждённый каскад mode_current, все UI). NOT restored on voltage
 * recovery (ТЗ). Reset to CMODE_TURBO only on driver off/on
 * (scan_voltage when !driver_is_active()).
 */
extern uint8_t volt_stop_mode;

/**
 * @brief Battery protection for the strobe driver (no DAC writes).
 *
 * Read-only NTC/Vrefint sample on each AWU wake, with the same four-sample
 * confirmation as scan_voltage(). Returns true after four consecutive
 * valid sub-2.7 V readings (including deep-discharge codes above 512,
 * vbat < 2.4 V) so the caller can abort the strobe and enter the
 * protective state.  The ADC is left running: it is powered down only by the
 * main-loop sleep block before STANDBY (stopping it here would leave a
 * powered-down ADC for the next sample_and_scan() on the awake path).
 * @return true when the strobe must be stopped.
 */
bool strobe_voltage_low(void);

/* ── USE_STROBE_ADC_GUARD: outcome of the strobe battery check ─────
 * Один замер на пробуждение, два независимых счётчика:
 *   - STROBE_BATT_LOW  — 4 валидных отсчёта ниже 2.70 В (как прежде,
 *     включает deep-discharge коды >512): необратимый down_forever();
 *   - STROBE_ADC_DEAD  — STROBE_ADC_FAIL_CONFIRM неудачных adc_read()
 *     подряд (таймаут/сбой): ВОССТАНОВИМЫЙ останов через off_strobe() —
 *     отказ измерений не доказывает разряд АКБ.
 * Любая валидная выборка сбрасывает fail_streak; срабатывание порога
 * сбрасывает соответствующий счётчик (свежий старт строба начинает
 * серию заново). */
#if USE_STROBE_ADC_GUARD
#define STROBE_BATT_OK     0u
#define STROBE_BATT_LOW    1u
#define STROBE_ADC_DEAD    2u
#define STROBE_ADC_FAIL_CONFIRM 3u

/**
 * @brief Combined strobe battery/measurement state (USE_STROBE_ADC_GUARD).
 * @return STROBE_BATT_OK, STROBE_BATT_LOW (confirmed deep discharge) or
 *         STROBE_ADC_DEAD (persistent acquisition failure).
 */
uint8_t strobe_battery_state(void);
#endif /* USE_STROBE_ADC_GUARD */

/**
 * @brief Irreversible shutdown: unpower load, mask interrupts, enter permanent STANDBY.
 */
void down_forever(void) __attribute__((noreturn));

#endif /* ADC_H_ */
