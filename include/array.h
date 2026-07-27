/*
 * array.h
 *
 *  Created on: Dec 24, 2022
 *      Author: tamag
 */

#ifndef INC_ARRAY_H_
#define INC_ARRAY_H_

/************************************************************************/
/*                                                                      */
/************************************************************************/
typedef enum {
	EXIT_MAGIC_MODE = 0U,
	SLOT_OP_EMPTY,
	SLOT_BATT_IND,
	SLOT_STROBE,
	SLOT_BLOCK_BEACON,
	SLOT_BLOCK,
	SLOT_TACTICAL,
	SLOT_BACKLIGHT,
	SLOT_BEACON,
	SLOT_EAUXMODE,
	SLOT_IPSMODE,
	SLOT_MEMORY,

	RESERV11,
} FUN_Index_t;

#endif /* INC_ARRAY_H_ */

extern void (*main_op[])(void);
