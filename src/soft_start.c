/*
 * soft_start.c — реализация плавного soft start
 *
 * Файл содержит две функции:
 * - ss_init() — вычисляет параметры перехода (направление, шаг, количество)
 * - ss_scan()  — выполняет один шаг перехода за каждый вызов
 *
 * Использование:
 *   ss_init(target, current, SS_FAST);  // начать переход
 *   // ... в loop() или timer ISR:
 *   ss_scan();  // вызывать пока ss.enable == true
 *
 * Пример: target=300, previous=100, speed=2 (SS_SLOW=1, SS_FAST=8)
 *   s_size = +2 (ramp-up)
 *   step_count = 200 / 2 = 100 шагов
 *   Каждый вызов ss_scan: position += 2, step_count--
 *   Через 100 вызовов: position = 300, enable = false
 */

#include <stdbool.h>
#include "soft_start.h"
#include "ui.h"
#include "proteus_config.h"

/* Глобальное состояние soft start (одна переменная — один процесс) */
ss_st ss;

/* Инициализация soft start: вычисление параметров перехода.
 *
 * target   — целевое значение яркости (куда хотим прийти)
 * previous — текущее значение яркости (откуда идём)
 * speed    — скорость перехода в шагах за вызов (SS_NO/SS_SLOW/SS_FAST)
 *
 * Алгоритм:
 * 1. Если target == previous — ничего не делаем (мы уже на месте)
 * 2. Вычисляем s_size: +speed (ramp-up) или -speed (ramp-down)
 * 3. Вычисляем step_count: расстояние / speed (количество вызовов ss_scan)
 * 4. Учитываем термоконтроль (get_step_tc) — реальный target может быть ниже MAX_STEP
 *
 * Важно: после вызова ss_init() нужно вызывать ss_scan() пока ss.enable == true */
void ss_init(uint16_t target, uint16_t previous, SoftStartSpeed_t speed)
{
	//EE_PRINTF("[SS] init target=%u prev=%u speed=%u\r\n", target, previous, speed);
	if (target == previous)
		return;                                     /* Уже на месте — soft start не нужен */

	/* s_size = направление × скорость:
	 *   ramp-up:   target > previous → +1 × speed = +speed
	 *   ramp-down: target < previous → -1 × speed = -speed */
	int8_t s_size = (target > previous ? 1 : -1) * speed;

	if (s_size > 0)
	{
		/* Ramp-up: target может быть ограничен термоконтролем */
		ss.target = get_step_tc(target);            /* Реальный target с учётом TC */
		uint16_t count = ABS((int16_t)(ss.target - previous)); /* Расстояние в шагах */
		ss.step_count = count ? count / speed : 1; /* Количество вызовов ss_scan до завершения задания*/
		ss.position = previous;                    /* Начинаем с текущей позиции */
	}
	else if (s_size < 0)
	{
		/* Ramp-down: previous может быть ограничен термоконтролем */
		ss.target = target;                        /* Цель — полное выключение или новая яркость */
		uint16_t tc_prev = get_step_tc(previous);  /* Текущая позиция с учётом TC */
		uint16_t count = ABS((int16_t)(target - tc_prev)); /* Расстояние в шагах */
		ss.step_count = count ? count / speed : 1; /* Количество вызовов ss_scan  до завершения задания*/
		ss.position = tc_prev;                     /* Начинаем с позиции TC */
	}

	ss.step_size = s_size;                         /* Запоминаем направление и скорость */
	ss.enable = true;                              /* Активируем soft start */
}

/* Один шаг soft start. Вызывается каждый тик (timer ISR или main loop).
 *
 * Алгоритм:
 * 1. Если ss.enable == false — ничего не делаем (soft start не активен)
 * 2. Если step_count > 0 — перемещаем позицию на step_size к цели
 *    - Ramp-up:   position += step_size (если не выходим за target)
 *    - Ramp-down: position += step_size (если не выходим за target)
 * 3. Уменьшаем step_count
 * 4. Если step_count == 0 — ставим position = target, отключаем soft start
 * 5. Вызываем out_set_stc(position) для применения яркости к DAC
 * 6. Обновляем step_tc для отслеживания текущей позиции другими модулями
 *
 * Вызывать пока ss.enable == true */
void ss_scan(void)
{
	if (ss.enable)
	{
		if (ss.step_count)
		{
			/* Ramp-up: двигаемся вверх, но не дальше target */
			if (ss.step_size > 0 && ss.position + ss.step_size <= ss.target)
				ss.position += ss.step_size;
			/* Ramp-down: двигаемся вниз, но не дальше target */
			else if (ss.step_size < 0 && ss.position + ss.step_size >= ss.target)
				ss.position += ss.step_size;
			ss.step_count--;                        /* Уменьшаем оставшиеся шаги */
		}

		if (!ss.step_count)
		{
			/* Все шаги выполнены — ставим точное целевое значение */
			ss.position = ss.target;
			ss.enable = false;                      /* Soft start завершён */
		}

		out_set_stc(ss.position);                   /* Применяем текущую позицию к DAC */
		step_tc = ss.position;                      /* Обновляем глобальную позицию яркости */
		
		if((!ss.enable) && (!step_tc)) OFF_POWER();	// выключаем питание DAC и I2C
	}
}
