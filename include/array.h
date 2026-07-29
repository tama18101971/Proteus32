/**
 * @file array.h
 * @brief Magic slot function index table
 *
 * Defines FUN_Index_t enum for identifying "magic slot" operations
 * (battery indicator, strobe, beacon, tactical, etc.).
 * main_op[] function pointer table is indexed by these values.
 */

#ifndef INC_ARRAY_H_
#define INC_ARRAY_H_

/** @brief Magic slot operation identifiers. */
typedef enum {
	EXIT_MAGIC_MODE = 0U, /**< Exit magic slot mode, return to normal UI */
	SLOT_OP_EMPTY,       /**< Empty/unconfigured slot */
	SLOT_BATT_IND,       /**< Battery indicator */
	SLOT_STROBE,        /**< Tactical strobe */
	SLOT_BLOCK_BEACON,  /**< Block beacon (block + beacon combo) */
	SLOT_BLOCK,          /**< Block / locator beacon */
	SLOT_TACTICAL,       /**< Tactical momentary */
	SLOT_BACKLIGHT,     /**< Backlight / auxiliary light */
	SLOT_BEACON,        /**< Beacon flash */
	SLOT_EAUXMODE,      /**< External aux mode */
	SLOT_IPSMODE,       /**< IPS (internal power supply) mode */
	SLOT_MEMORY,        /**< Memory recall */
	RESERV11,           /**< Reserved */
} FUN_Index_t;

/** @brief Function pointer table for magic slot dispatch, defined in ui.c. */
extern void (*main_op[])(void);

#endif /* INC_ARRAY_H_ */
