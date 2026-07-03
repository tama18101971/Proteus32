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

#endif /* ADC_H_ */