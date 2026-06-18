/*
 * soft_start.c
 *
 *  Created on: Dec 25, 2022
 *      Author: tamag
 */

#include "stdbool.h"
#include "soft_start.h"
#include "button.h"
#include "config.h"

ss_st ss;

void ss_init(uint16_t target, uint16_t previous, SoftStartSpeed_t speed) //  target - куда идем, step - текущее положение
{
	EE_PRINTF("[SS] init target=%u prev=%u speed=%u\r\n", target, previous, speed);
	if (target == previous)
		return;

	int8_t s_size = (target > previous ? 1 : -1) * speed; // вычисляем направление и умножаем на скорость.

	if (s_size > 0)
	{
		ss.target = get_step_tc(target);
		uint16_t count = ABS((int16_t)(ss.target - previous));
		ss.step_count = count ? count / speed : 1;
		ss.position = previous;
	}
	else if (s_size < 0)
	{
		ss.target = target;
		uint16_t tc_prev = get_step_tc(previous);
		uint16_t count = ABS((int16_t)(target - tc_prev));
		ss.step_count = count ? count / speed : 1;
		ss.position = tc_prev;
	}

	ss.step_size = s_size;
	ss.enable = true;
}

void ss_scan(void)
{
	if (ss.enable) // Тут все ясно
	{
		if (ss.step_count)
		{
			if (ss.step_size > 0 && ss.position + ss.step_size <= ss.target)
				ss.position += ss.step_size;
			else if (ss.step_size < 0 && ss.position + ss.step_size >= ss.target)
				ss.position += ss.step_size;
			ss.step_count--;
		}

		if (!ss.step_count)
		{							 // Если 0 - заканчиваем
			ss.position = ss.target; // Ставим в желательное значение
			ss.enable = false;		 // Все сделали - отключаем
		}

		// sleep_remains = SLEEP_DELAY;										// Не даем уснуть в процессе

		out_set_stc(ss.position);
		step_tc = ss.position;
	}
}
