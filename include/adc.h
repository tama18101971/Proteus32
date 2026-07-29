#ifndef ADC_H_
#define ADC_H_

/*
 * adc.h — АЦП для измерения температуры NTC и напряжения питания
 *
 * АЦП (ADC1) используется для:
 *   - Измерения температуры NTC-термистора (канал 3, PD2)
 *   - Измерения напряжения питания через Vrefint (канал 8)
 *
 * Функции:
 *   adc_init()  — полная инициализация АЦП с калибровкой
 *   adc_read()  — одно измерение (NTC + Vrefint), вывод результатов
 *   adc_stop()  — полная остановка АЦП (тактирование off, GPIO Hi-Z)
 *
 * EMA-фильтр (Exponential Moving Average):
 *   ema_init()  — начальная инициализация фильтра
 *   ema_update() — обновление фильтра новым значением
 *
 * Температурная таблица: -20..+80°C, шаг 1°C
 */

#include <stdint.h>

#define TEMPERATURE_TABLE_START  (-200)
#define TEMPERATURE_TABLE_STEP   (10)

void adc_read(void);
void adc_init(void);
void adc_stop(void);

int16_t calc_temperature(uint16_t adcValue);

/* EMA-фильтр для Vrefint ADC (измерение напряжения Vdd).
 * shift=4: alpha=1/16, запаздывание ~16 отсчётов.
 * shift=3: alpha=1/8,  запаздывание ~8 отсчётов.
 * Больше shift — плавнее, но медленнее реакция. */
#define EMA_SHIFT 4
void     ema_init(uint16_t seed);
uint16_t ema_update(uint16_t raw);

extern uint16_t vbat_mv;  /* Напряжение питания в мВ */
extern int16_t temperature_c;
extern uint16_t raw_temperature_adc; /* Сырое значение температуры с АЦП */

uint8_t bat_get_flashes(void);  /* Количество вспышек по заряду батареи (0..6) */

/* ============ Калибровка термоконтроля ============ */

#define REF_TEMP_ADC    542    /* Ожидаемый ADC при 25°C — termo_table[45] */
#define REF_VOLTAGE_MV  4300   /* Ожидаемое Vbat при калибровке (мВ) */

#define VC_CLIP_MAX     127    /* Максимальное значение vc (int8_t, ±127) */
#define VC_CLIP_MIN     -127
#define TC_CLIP_MAX     31     /* Максимальное значение tc */
#define TC_CLIP_MIN     -31

#define ADC_MIN_VAL     0      /* Диапазон 10-бит АЦП */
#define ADC_MAX_VAL     1023

uint16_t get_exact_temp_adc(void);  /* raw_temperature_adc - tc, clamped 0..1023 */

/* ============ Защита по разряду батареи ============ */

void scan_voltage(void);              /* Основной цикл защиты, вызывать 30 Гц после adc_read() */

extern uint8_t volt_max_mode;         /* Динамический потолок режима UI2 (обновляется в scan_voltage) */
extern uint8_t volt_stop_mode;        /* Ратчет-память — исторический минимум volt_max_mode */

extern uint8_t down_count;           /* Счётчик ступеней UI1 (сбрасывается в on_driver) */
extern uint16_t max_min;              /* Монотонно убывающий потолок шага UI1 по напряжению */

uint16_t get_max_step(void);         /* Потолок яркости UI1 по напряжению (монотонный, аналог AVR) */

#endif /* ADC_H_ */