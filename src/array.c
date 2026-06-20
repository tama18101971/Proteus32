/*
 * array.c
 *
 *  Created on: Dec 24, 2022
 *      Author: tamag
 */

#include "ui.h"

/************************************************************************/
/*                                                                      */
/************************************************************************/

void (*main_op[])(void) =
{
	/*00*/	0, // exit_magic_mode,
	/*01*/	0, // slot_empty,
	/*02*/	0, // slot_batt_ind,
	/*03*/	0, // slot_strobe,
	/*04*/	0, // slot_block_beacon,
	/*05*/	0, // slot_block,
	/*06*/	0, // slot_tactical,
	/*07*/	0, // slot_backlight,
	/*08*/	0, // slot_beacon,
	/*09*/	0, // slot_eauxmode,
	/*10*/	0, // slot_ipsmode,
	/*11*/	0, // slot_memory,
	/*12*/	0
};

