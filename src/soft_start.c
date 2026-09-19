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
#include "limiters.h"
#include "proteus_config.h"

/* Q15-скейлер count/7 для SS_FAST: на единицу больше точного 32768/7 = 4681.43.
 * Заниженное значение давало step_count на 1 тик меньше идеала при расстояниях,
 * кратных 7, и финальное точное присваивание превращалось в видимый рывок +7. */
#define SS_FAST_SCALER  4682u

/** @brief Global soft start state. */
ss_st ss;

/**
 * @brief Initialise soft start transition.
 * @param target   Already thermal-limited physical target brightness.
 * @param previous Current brightness.
 * @param speed    Transition speed (SS_NO = instant).
 *
 * If target == previous, soft start is skipped (already at target).
 * SS_NO also cancels any pending transition (instant transitions are
 * applied by the caller side without entering the FSM).
 * The caller resolves thermal limiting before entering this module. This
 * retains the pre-FSM Proteus32 profile: step_size is +/- speed and
 * step_count is distance / speed.  The final scan assigns target exactly,
 * so truncation never leaves a transition one or more steps short.
 */
void ss_init(uint16_t target, uint16_t previous, SoftStartSpeed_t speed)
{
	if ((target == previous) || (speed == SS_NO))
	{
		ss_cancel();
		return;
	}

	/* set_current() resolves the thermal limit before entering this
	 * low-level module, so target is already the physical safe target. */
	ss.target = target;
	uint16_t count = ABS((int16_t)(target - previous));
	uint16_t sc = (speed == SS_FAST) ? (uint16_t)(((uint32_t)count * SS_FAST_SCALER) >> 15) : count;
	ss.step_count = sc ? sc : 1;
	ss.position = previous;
	/* speed != SS_NO here (early return above), so step_size is never 0. */
	ss.step_size = (target > previous) ? (int8_t)speed : (int8_t)-(int8_t)speed;
	ss.enable = true;
}

/**
 * @brief Execute one soft start step (call at 160 Hz).
 *
 * Moves position by step_size toward target. When step_count reaches 0,
 * position is set to target exactly and enable is cleared.
 * Calls out_set_stc() to apply brightness, updates step_tc.
 * Calls OFF_POWER() only when the user request is also zero; a thermal
 * limiter may legitimately clamp a non-zero request to physical step 0 while
 * keeping the DAC powered for later recovery.
 */
void ss_scan(void)
{
	if (ss.enable)
	{
		uint16_t ceil = get_step_tc(step_user);

		if (ss.target > ceil)
			ss.target = ceil;
		if (ss.step_size > 0 && ss.position > ceil)
			ss.position = ceil;

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
		
		if((!ss.enable) && (!step_tc) && (!step_user)) OFF_POWER();
	}
}

/**
 * @brief Cancel a pending soft-start transition without changing the output.
 */
void ss_cancel(void)
{
	ss.enable = false;
	ss.step_count = 0;
}
