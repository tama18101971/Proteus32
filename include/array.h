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
	SLOT_OP_EMPTY,        /**< Empty/unconfigured slot */
	SLOT_BATT_IND,        /**< Battery indicator */
	SLOT_STROBE,          /**< Tactical strobe */
	SLOT_BLOCK_BEACON,    /**< Block beacon (block + beacon combo) */
	SLOT_BLOCK,           /**< Block / locator beacon */
	SLOT_TACTICAL,        /**< Tactical momentary */
	SLOT_BACKLIGHT,       /**< Backlight / auxiliary light */
	SLOT_BEACON,          /**< Beacon flash */
	SLOT_EAUXMODE,        /**< Extended aux mode */
	SLOT_IPSMODE,         /**< IPS (Red Grin + HBL) mode */
	SLOT_MEMORY,          /**< Memory recall */
	SLOT_UI_SELECT,       /**< Quick UI selection (1/2/3 clicks) */
} FUN_Index_t;

#define SLOT_OP_FIRST  SLOT_OP_EMPTY
#define SLOT_OP_LAST   SLOT_UI_SELECT

/*
 * Fixed commands for the ten OFF-state slot gestures.  These are also the
 * factory contents programmed into Option Bytes.  Keeping this mapping here
 * makes the non-configurable build behave exactly like a reset magic-slot
 * configuration, without taking a dependency on Option Bytes at runtime.
 */
#define SLOT_DEFAULT_1CH  SLOT_BATT_IND
#define SLOT_DEFAULT_2CH  SLOT_BEACON
#define SLOT_DEFAULT_3C   SLOT_STROBE
#define SLOT_DEFAULT_3CH  SLOT_TACTICAL
#define SLOT_DEFAULT_4C   SLOT_BLOCK
#define SLOT_DEFAULT_4CH  SLOT_OP_EMPTY
#define SLOT_DEFAULT_5C   SLOT_OP_EMPTY
#define SLOT_DEFAULT_5CH  SLOT_MEMORY
#define SLOT_DEFAULT_6C   SLOT_BACKLIGHT
#define SLOT_DEFAULT_6CH  SLOT_IPSMODE

/** @brief Function pointer table for magic slot dispatch, defined in ui_table.c. */
extern void (* const main_op[])(void);

#endif /* INC_ARRAY_H_ */
