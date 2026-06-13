/*
 * soft_start.h
 *
 *  Created on: Dec 25, 2022
 *      Author: tamag
 */

#ifndef INC_SOFT_START_H_
#define INC_SOFT_START_H_

#include "stdint.h"

#define ABS(x)				((x>0)?(x):(-x))

typedef enum {
	SS_NO = 0,
	SS_SLOW,
	SS_FAST = 8
} SoftStartSpeed_t;

#define on_ss() do { ss_enable = true; } while (0);

// soft start
typedef struct {
	uint8_t enable;
	int8_t step_size;			// на сколько изменять
	uint16_t target;			// желаемое положение
	uint16_t step_count;		// сколько раз изменять
	uint16_t position;			// конкретная позиция яркости
} ss_st;

void scan_ss(void);
void ss_init(uint16_t target, uint16_t previous, SoftStartSpeed_t speed);

#endif /* INC_SOFT_START_H_ */
