/*
 * ui_table.h
 *
 * Dispatch table types and lookup.
 *
 * Maps (driver_mode + button_event) to action functions.
 * There is one dispatch table per UI type (ramping, discrete, engineering
 * and user menus).
 *
 * Each table entry (dispatch_t) specifies:
 *   - which driver_mode to match
 *   - how many clicks the event must have
 *   - whether it fires on hold or release
 *   - whether it needs the CLICK_SEQ timeout
 *   - which action function to call (via main_op[] index)
 */

#ifndef UI_TABLE_H_
#define UI_TABLE_H_

#include <stdint.h>
#include <stdbool.h>

#include "button.h"

/*******************************************************************************
 *  Dispatch entry structure
 ******************************************************************************/

typedef struct {
    uint8_t mode;            // driver_mode to match
    uint8_t clicks;          // click count to match (0 = wildcard)
    uint8_t long_mask;       // required long-click bits (0 = don't care)
    uint8_t hold       : 1;  // true = match hold events; false = releases
    uint8_t wait_timeout : 1; // true = fire only on CLICK_SEQ timeout
    uint8_t op_index   : 6;  // fixed fallback index into main_op[] (0..48)
    uint8_t is_slot    : 1;  // true = a slot gesture; magic overrides op_index
    uint8_t slot_idx   : 4;  // Option Bytes slot index (0..9)
    uint8_t reserved   : 3;
} dispatch_t;

_Static_assert(sizeof(dispatch_t) == 5,
               "dispatch_t packing changed; update UI table size assumptions");

/*******************************************************************************
 *  Lookup function
 ******************************************************************************/

/**
 * @brief Look up and dispatch a button event in the active UI table.
 * @param mode    Current driver_mode.
 * @param evt     Current button event.
 * @param timeout True if the click-sequence timeout has elapsed.
 * @return 1 if an action was dispatched, 0 if no match found.
 */
uint8_t ui_table_lookup(uint8_t mode, const btn_evt_t *evt, bool timeout);

/*******************************************************************************
 *  main_op index constants
 ******************************************************************************/

/*
 *  Slot content op-codes (0..11) are defined in array.h as FUN_Index_t enum:
 *    EXIT_MAGIC_MODE = 0, SLOT_EMPTY, SLOT_BATT_IND, ... SLOT_MEMORY
 */

/* -- Reserved: 12 -- */

/* -- UI1 strobe / block (13..17) -- */
#define UP_STROBE_UI1       13
#define BACK_STROBE_UI1     14
#define OFF_STROBE_UI1      15
#define UNBLOCK_DRV         16
#define OFF_DRV_FALSE       17

/* -- UI1 brightness / mode (18..22) -- */
#define ON_XMODE_UI1        18
#define ON_RUMPUP_UI1       19
#define ON_RUMPDOWN_UI1     20
#define ON_TURBO_UI1        21
#define CALIBRATION         22

/* -- Engineering menu: enter (23..25) -- */
#define ENTER_ML_MODE       23
#define SAVE_DEF_UI1        24
#define SAVE_DEF_UI2        25

/* -- Engineering menu: click actions (26) -- */
#define ML_CLICKX           26

/* -- Menu exit (27) -- */
#define MENU_EXIT           27

/* -- User menu: thermocontrol (28..29) -- */
#define ENTER_CHTK_MODE     28
#define CHTK_CLICKX         29

/* -- User menu: beacon mode (30..31) -- */
#define ENTER_BM_MODE       30
#define BM_CLICKX           31

/* -- User menu: beacon type (32..33) -- */
#define ENTER_BT_MODE       32
#define BT_CLICKX           33

/* -- User menu: backlight (34..35) -- */
#define ENTER_BL_MODE       34
#define BL_CLICKX           35

/* -- Magic slot (36) -- */
#define ENTER_MAGIC_MODE    36

/* -- Shared magic-slot handlers (37..38) -- */
#define SELECT_SLOT         37
#define SLOT_CLICKX         38

/* -- UI2 actions (39..48) -- */
#define MODE_UP_UI2         39
#define ON_TURBO_UI2        40
#define ON_MIN_UI2          41
#define ON_MOON_UI2         42
#define ON_XMODE_UI2        43
#define MODE_DOWN_UI2       44
#define STEP_DOWN_UI2       45
#define ENTER_UIS_MODE      46
#define UIS_CLICKX          47
#define RESET2DEF           48

/*******************************************************************************
 *  Main action function table
 ******************************************************************************/

extern void (*main_op[])(void);

#endif /* UI_TABLE_H_ */
