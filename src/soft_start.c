/**
 * @file soft_start.c
 * @brief Smooth brightness transitions (soft start)
 *
 * Two functions:
 * - ss_init() — computes transition parameters (direction, step size, count)
 * - ss_scan()  — executes one step per call
 *
 * Usage:
 *   ss_init(target, current, SS_FAST);
 *   while (ss.enable) { ss_scan(); }
 *
 * Example: target=300, previous=100, speed=SS_FAST (7)
 *   step_size = +7 (ramp-up)
 *   step_count = 200 / 7 = 28 scans, then target is applied exactly.
 */

#include <stdbool.h>
#include "soft_start.h"
#include "ui.h"
#include "proteus_config.h"

/** @brief Global soft start state. */
ss_st ss;

/**
 * @brief Initialise soft start transition.
 * @param target   Target brightness.
 * @param previous Current brightness.
 * @param speed    Transition speed (SS_NO = instant).
 *
 * If target == previous, soft start is skipped (already at target).
 * This retains the pre-FSM Proteus003 profile: step_size is +/- speed and
 * step_count is distance / speed.  The final scan assigns target exactly,
 * so truncation never leaves a transition one or more steps short.
 */
void ss_init(uint16_t target, uint16_t previous, SoftStartSpeed_t speed)
{
	if (target == previous)
		return;

	int8_t s_size = (target > previous ? 1 : -1) * speed;

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

/**
 * @brief Execute one soft start step (call at 160 Hz).
 *
 * Moves position by step_size toward target. When step_count reaches 0,
 * position is set to target exactly and enable is cleared.
 * Calls out_set_stc() to apply brightness, updates step_tc.
 * Calls OFF_POWER() if brightness reaches 0.
 */
void ss_scan(void)
{
	if (ss.enable)
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
		{
			ss.position = ss.target;
			ss.enable = false;
		}

		out_set_stc(ss.position);
		step_tc = ss.position;
		
		if((!ss.enable) && (!step_tc)) OFF_POWER();
	}
}
