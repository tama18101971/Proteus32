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

/* -- Reserved: 12..20 -- */

/* -- UI1 strobe / block (21..25) -- */
#define UP_STROBE_UI1       21
#define BACK_STROBE_UI1     22
#define OFF_STROBE_UI1      23
#define UNBLOCK_DRV         24
#define OFF_DRV_FALSE       25

/* -- UI1 brightness / mode (26..30) -- */
#define ON_XMODE_UI1        26
#define ON_RUMPUP_UI1       27
#define ON_RUMPDOWN_UI1     28
#define ON_TURBO_UI1        29
#define CALIBRATION         30

/* -- Engineering menu: enter (31..35) -- */
#define ENTER_RS_MODE       31
#define ENTER_ML_MODE       32
#define ENTER_PT_MODE       33
#define SAVE_DEF_UI1        34
#define SAVE_DEF_UI2        35

/* -- Engineering menu: click actions (36..38) -- */
#define RS_CLICKX           36
#define ML_CLICKX           37
#define PT_CLICKX           38

/* -- Menu exit (39) -- */
#define MENU_EXIT           39

/* -- User menu: thermocontrol (40..41) -- */
#define ENTER_CHTK_MODE     40
#define CHTK_CLICKX         41

/* -- User menu: beacon mode (42..43) -- */
#define ENTER_BM_MODE       42
#define BM_CLICKX           43

/* -- User menu: beacon type (44..45) -- */
#define ENTER_BT_MODE       44
#define BT_CLICKX           45

/* -- User menu: backlight (46..47) -- */
#define ENTER_BL_MODE       46
#define BL_CLICKX           47

/* -- Magic slot (48) -- */
#define ENTER_MAGIC_MODE    48

/* -- Shared magic-slot handlers (49..50) -- */
#define SELECT_SLOT         49
#define SLOT_CLICKX         50

/* -- UI2 actions (51..60) -- */
#define MODE_UP_UI2         51
#define ON_TURBO_UI2        52
#define ON_MIN_UI2          53
#define ON_MOON_UI2         54
#define ON_XMODE_UI2        55
#define MODE_DOWN_UI2       56
#define STEP_DOWN_UI2       57
#define ENTER_UIS_MODE      58
#define UIS_CLICKX          59
#define RESET2DEF           60

/*******************************************************************************
 *  Main action function table
 ******************************************************************************/

extern void (*main_op[])(void);

#endif /* UI_TABLE_H_ */
