/**
 * @file soft_start.h
 * @brief Smooth brightness transitions (soft start)
 *
 * Provides gradual step-up/step-down of brightness from current value
 * to a target over a configurable number of steps. Called from main loop
 * via ss_scan() at 160 Hz.
 *
 * Algorithm:
 * 1. ss_init() computes step size, direction and step count
 * 2. ss_scan() moves position one step toward target on each call
 * 3. On target reached: ss.enable = false, position = target
 */

#ifndef INC_SOFT_START_H_
#define INC_SOFT_START_H_

#include "stdint.h"

/** @brief Absolute value macro (simple expressions only). */
#define ABS(x)				((x>0)?(x):(-x))

/**
 * @brief Soft start speed (steps per ss_scan call).
 */
typedef enum {
	SS_NO = 0,  /**< Instant transition (no smoothing) */
	SS_SLOW,    /**< Slow ramp: 1 step per call */
	SS_FAST = 7 /**< Fast ramp: 8 steps per call */
} SoftStartSpeed_t;

/** @brief Enable soft start processing. */
#define on_ss() do { ss_enable = true; } while (0)

/**
 * @brief Soft start state structure.
 *
 * Initialised by ss_init(), consumed by ss_scan().
 */
typedef struct {
	uint8_t  enable;      /**< Activity flag: 1=active, 0=done */
	int8_t   step_size;   /**< Step delta: +speed for ramp-up, -speed for ramp-down */
	uint16_t target;      /**< Target brightness (destination) */
	uint16_t step_count;  /**< Remaining steps (decremented in ss_scan) */
	uint16_t position;    /**< Current brightness position */
} ss_st;

/**
 * @brief Initialise soft start transition.
 * @param target   Target brightness value.
 * @param previous Current brightness value.
 * @param speed    Transition speed (SS_NO, SS_SLOW, SS_FAST).
 */
void ss_init(uint16_t target, uint16_t previous, SoftStartSpeed_t speed);

/**
 * @brief Called every 160 Hz tick — advance position toward target.
 */
void ss_scan(void);

#endif /* INC_SOFT_START_H_ */
