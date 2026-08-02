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
    bool    hold;            // true = match hold events; false = match releases
    bool    wait_timeout;    // true = fire only on CLICK_SEQ timeout
    uint8_t op_index;        // fixed fallback index into main_op[]
    bool    is_slot;         // true = a slot gesture; magic build overrides op_index
    uint8_t slot_idx;        // Option Bytes slot index (0..9)
} dispatch_t;

/*******************************************************************************
 *  Lookup function
 ******************************************************************************/

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

/* -- Engineering menu: enter (23..27) -- */
#define ENTER_RS_MODE       23
#define ENTER_ML_MODE       24
#define ENTER_PT_MODE       25
#define SAVE_DEF_UI1        26
#define SAVE_DEF_UI2        27

/* -- Engineering menu: click actions (28..30) -- */
#define RS_CLICKX           28
#define ML_CLICKX           29
#define PT_CLICKX           30

/* -- Menu exit (31) -- */
#define MENU_EXIT           31

/* -- User menu: thermocontrol (32..33) -- */
#define ENTER_CHTK_MODE     32
#define CHTK_CLICKX         33

/* -- User menu: beacon mode (34..35) -- */
#define ENTER_BM_MODE       34
#define BM_CLICKX           35

/* -- User menu: beacon type (36..37) -- */
#define ENTER_BT_MODE       36
#define BT_CLICKX           37

/* -- User menu: backlight (38..39) -- */
#define ENTER_BL_MODE       38
#define BL_CLICKX           39

/* -- Magic slot (40) -- */
#define ENTER_MAGIC_MODE    40

/* -- Shared magic-slot handlers (41..42) -- */
#define SELECT_SLOT         41
#define SLOT_CLICKX         42

/* -- UI2 actions (43..52) -- */
#define MODE_UP_UI2         43
#define ON_TURBO_UI2        44
#define ON_MIN_UI2          45
#define ON_MOON_UI2         46
#define ON_XMODE_UI2        47
#define MODE_DOWN_UI2       48
#define STEP_DOWN_UI2       49
#define ENTER_UIS_MODE      50
#define UIS_CLICKX          51
#define RESET2DEF           52

/*******************************************************************************
 *  Main action function table
 ******************************************************************************/

extern void (*main_op[])(void);

#endif /* UI_TABLE_H_ */
