/*
 * ui_table.c
 *
 * Dispatch tables + main_op[] function pointer array.
 *
 * Ported from Proteus84 (AVR / PROGMEM) to CH32V003 RAM.
 */

#include <stddef.h>

#include "proteus_config.h"
#include "ui_table.h"
#include "ui.h"
#include "app_data.h"
#include "array.h"
#include "ob.h"

#define MAIN_OP_COUNT 79

/* Current UI index used for table selection. Defined in ui.c. */
extern uint8_t ui_table_idx;

/*******************************************************************************
 *  Slot indices
 ******************************************************************************/

enum {
    SLOT_1CH = 0,
    SLOT_2CH = 1,
    SLOT_3C  = 2,
    SLOT_3CH = 3,
    SLOT_4C  = 4,
    SLOT_4CH = 5,
    SLOT_5C  = 6,
    SLOT_5CH = 7,
    SLOT_6C  = 8,
    SLOT_6CH = 9,
};

/* In the fixed build default_op is used.  In the magic build lookup replaces
 * it with Option Bytes slot_idx.  is_slot remains true in both builds so a
 * hold gesture stays a one-shot command. */
#define SLOT_ACTION(default_op, slot_idx)  (default_op), true, (slot_idx)

/*******************************************************************************
 *  UI1 dispatch table (ramping interface)
 ******************************************************************************/

static const dispatch_t ui1_table[] = {
    /* -- Mode: MODE_OFF -- */
    { MODE_OFF, 1, 0, false, true,  ON_XMODE_UI1,  false, 0 },
    { MODE_OFF, 1, 0, true,  false, SLOT_ACTION(SLOT_DEFAULT_1CH, SLOT_1CH) },
    { MODE_OFF, 2, 0, false, true,  ON_TURBO_UI1,   false, 0 },
    { MODE_OFF, 2, 0, true,  false, SLOT_ACTION(SLOT_DEFAULT_2CH, SLOT_2CH) },
    { MODE_OFF, 3, 0, false, true,  SLOT_ACTION(SLOT_DEFAULT_3C, SLOT_3C) },
    { MODE_OFF, 3, 0, true,  false, SLOT_ACTION(SLOT_DEFAULT_3CH, SLOT_3CH) },
    { MODE_OFF, 4, 0, false, true,  SLOT_ACTION(SLOT_DEFAULT_4C, SLOT_4C) },
    { MODE_OFF, 4, 0, true,  false, SLOT_ACTION(SLOT_DEFAULT_4CH, SLOT_4CH) },
    { MODE_OFF, 5, 0, false, true,  SLOT_ACTION(SLOT_DEFAULT_5C, SLOT_5C) },
    { MODE_OFF, 5, 0, true,  false, SLOT_ACTION(SLOT_DEFAULT_5CH, SLOT_5CH) },
    { MODE_OFF, 6, 0, false, true,  SLOT_ACTION(SLOT_DEFAULT_6C, SLOT_6C) },
    { MODE_OFF, 6, 0, true,  false, SLOT_ACTION(SLOT_DEFAULT_6CH, SLOT_6CH) },
    { MODE_OFF, 0, 0, true,  false, ON_RUMPUP_UI1,  false, 0 },

    /* -- Mode: MODE_ON -- */
    { MODE_ON,  1, 0, false, true,  OFF_DRV_FALSE,  false, 0 },
    { MODE_ON,  0, 0, true,  false, ON_RUMPUP_UI1,  false, 0 },
    { MODE_ON,  1, 0, true,  false, ON_RUMPDOWN_UI1, false, 0 },
    { MODE_ON,  2, 0, false, true,  ON_TURBO_UI1,   false, 0 },
    { MODE_ON,  5, 0, true,  false, SAVE_DEF_UI1,   false, 0 },

    /* -- Mode: MODE_STROB -- */
    { MODE_STROB, 1, 0, false, true,  OFF_STROBE_UI1, false, 0 },
    { MODE_STROB, 2, 0, false, true,  UP_STROBE_UI1,  false, 0 },

    /* -- Mode: MODE_BLOCK / MODE_BLOCK0 -- */
    { MODE_BLOCK,  6, 0, false, false, UNBLOCK_DRV,   false, 0 },
    { MODE_BLOCK0, 6, 0, false, false, UNBLOCK_DRV,   false, 0 },

    /* -- Fallback unblock for non-magic slot configuration -- */
    { MODE_BLOCK,  4, 0, false, false, UNBLOCK_DRV,   false, 0 },
    { MODE_BLOCK0, 4, 0, false, false, UNBLOCK_DRV,   false, 0 },
};

#define UI1_TABLE_SIZE (sizeof(ui1_table) / sizeof(ui1_table[0]))

/*******************************************************************************
 *  UI2 dispatch table (discrete brightness interface)
 ******************************************************************************/

static const dispatch_t ui2_table[] = {
    /* -- Mode: MODE_OFF -- */
    { MODE_OFF, 1, 1, false, false, ON_MIN_UI2,     false, 0 },
    { MODE_OFF, 1, 0, false, true,  ON_XMODE_UI2,   false, 0 },
    { MODE_OFF, 1, 0, true,  false, SLOT_ACTION(SLOT_DEFAULT_1CH, SLOT_1CH) },
    { MODE_OFF, 2, 0, false, true,  ON_TURBO_UI2,   false, 0 },
    { MODE_OFF, 2, 0, true,  false, SLOT_ACTION(SLOT_DEFAULT_2CH, SLOT_2CH) },
    { MODE_OFF, 3, 0, false, true,  SLOT_ACTION(SLOT_DEFAULT_3C, SLOT_3C) },
    { MODE_OFF, 3, 0, true,  false, SLOT_ACTION(SLOT_DEFAULT_3CH, SLOT_3CH) },
    { MODE_OFF, 4, 0, false, true,  SLOT_ACTION(SLOT_DEFAULT_4C, SLOT_4C) },
    { MODE_OFF, 4, 0, true,  false, SLOT_ACTION(SLOT_DEFAULT_4CH, SLOT_4CH) },
    { MODE_OFF, 5, 0, false, true,  SLOT_ACTION(SLOT_DEFAULT_5C, SLOT_5C) },
    { MODE_OFF, 5, 0, true,  false, SLOT_ACTION(SLOT_DEFAULT_5CH, SLOT_5CH) },
    { MODE_OFF, 6, 0, false, true,  SLOT_ACTION(SLOT_DEFAULT_6C, SLOT_6C) },
    { MODE_OFF, 6, 0, true,  false, SLOT_ACTION(SLOT_DEFAULT_6CH, SLOT_6CH) },
    { MODE_OFF, 0, 0, true,  false, ON_MOON_UI2,    false, 0 },

    /* -- Mode: MODE_ON -- */
    { MODE_ON,  1, 1, false, false, STEP_DOWN_UI2,  false, 0 },
    { MODE_ON,  1, 0, false, true,  OFF_DRV_FALSE,  false, 0 },
    { MODE_ON,  0, 0, true,  false, MODE_UP_UI2,    false, 0 },
    { MODE_ON,  1, 0, true,  false, MODE_DOWN_UI2,  false, 0 },
    { MODE_ON,  2, 0, false, true,  ON_TURBO_UI2,   false, 0 },

    /* -- Mode: MODE_STROB -- */
    { MODE_STROB, 1, 0, false, true,  OFF_STROBE_UI1, false, 0 },
    { MODE_STROB, 2, 0, false, true,  UP_STROBE_UI1,  false, 0 },

    /* -- Mode: MODE_BLOCK -- */
    { MODE_BLOCK,  6, 0, false, false, UNBLOCK_DRV,   false, 0 },
    { MODE_BLOCK0, 6, 0, false, false, UNBLOCK_DRV,   false, 0 },
};

#define UI2_TABLE_SIZE (sizeof(ui2_table) / sizeof(ui2_table[0]))

/*******************************************************************************
 *  Engineering menu dispatch table (ui_index == 5 / ENG_MENU_IND)
 ******************************************************************************/

static const dispatch_t eng_table[] = {
    /* -- First level: MODE_OFF (navigate sub-menus) -- */
    { MODE_OFF, 1,  0, false, true,  ENTER_RS_MODE,  false, 0 },
    { MODE_OFF, 2,  0, false, true,  ENTER_ML_MODE,  false, 0 },
    { MODE_OFF, 3,  0, false, true,  ENTER_PT_MODE,  false, 0 },
    { MODE_OFF, 4,  0, false, true,  SAVE_DEF_UI1,   false, 0 },
    { MODE_OFF, 5,  0, false, true,  SAVE_DEF_UI2,   false, 0 },
    { MODE_OFF, 10, 0, false, true,  CALIBRATION,    false, 0 },
    { MODE_OFF, 20, 0, false, false, RESET2DEF,      false, 0 },
    { MODE_OFF, 0,  0, true,  false, MENU_EXIT,      false, 0 },

    /* -- Sub-modes -- */
    { MODE_RAMP_SPEED, 0, 0, false, true,  RS_CLICKX,  false, 0 },
    { MODE_RAMP_SPEED, 0, 0, true,  false, MENU_EXIT,  false, 0 },

    { MODE_MOON_LEVEL, 0, 0, false, true,  ML_CLICKX,  false, 0 },
    { MODE_MOON_LEVEL, 0, 0, true,  false, MENU_EXIT,  false, 0 },

    { MODE_PID_TIME,   0, 0, false, true,  PT_CLICKX,  false, 0 },
    { MODE_PID_TIME,   0, 0, true,  false, MENU_EXIT,  false, 0 },

    /* -- Block/unblock -- */
    { MODE_BLOCK,  6, 0, false, false, UNBLOCK_DRV,   false, 0 },
    { MODE_BLOCK0, 6, 0, false, false, UNBLOCK_DRV,   false, 0 },
};

#define ENG_TABLE_SIZE (sizeof(eng_table) / sizeof(eng_table[0]))

/*******************************************************************************
 *  User settings menu dispatch table (ui_index == 6 / USR_MENU_IND)
 ******************************************************************************/

static const dispatch_t usr_table[] = {
    /* -- First level: MODE_OFF -- */
    { MODE_OFF, 1, 0, false, true,  ENTER_BM_MODE,   false, 0 },
    { MODE_OFF, 2, 0, false, true,  ENTER_BT_MODE,   false, 0 },
    { MODE_OFF, 3, 0, false, true,  ENTER_BL_MODE,   false, 0 },
    { MODE_OFF, 4, 0, false, true,  ENTER_UIS_MODE,  false, 0 },
    { MODE_OFF, 5, 0, false, true,  ENTER_MAGIC_MODE, false, 0 },
    { MODE_OFF, 6, 0, false, true,  ENTER_CHTK_MODE, false, 0 },
    { MODE_OFF, 0, 0, true,  false, MENU_EXIT,       false, 0 },

    /* -- Sub-modes -- */
    { MODE_BM,  0, 0, false, true,  BM_CLICKX,  false, 0 },
    { MODE_BM,  0, 0, true,  false, MENU_EXIT,  false, 0 },

    { MODE_BT,  0, 0, false, true,  BT_CLICKX,  false, 0 },
    { MODE_BT,  0, 0, true,  false, MENU_EXIT,  false, 0 },

    { MODE_BL,  0, 0, false, true,  BL_CLICKX,  false, 0 },
    { MODE_BL,  0, 0, true,  false, MENU_EXIT,  false, 0 },

    { MODE_UIS, 0, 0, false, true,  UIS_CLICKX, false, 0 },
    { MODE_UIS, 0, 0, true,  false, MENU_EXIT,  false, 0 },

    { MODE_CTK_H, 0, 0, false, true,  CHTK_CLICKX, false, 0 },
    { MODE_CTK_H, 0, 0, true,  false, MENU_EXIT,   false, 0 },

    /* -- Magic mode -- */
    { MODE_MAGIC, 1,  0, false, true,  SELECT_SLOT01, false, 0 },
    { MODE_MAGIC, 2,  0, false, true,  SELECT_SLOT02, false, 0 },
    { MODE_MAGIC, 3,  0, false, true,  SELECT_SLOT03, false, 0 },
    { MODE_MAGIC, 4,  0, false, true,  SELECT_SLOT04, false, 0 },
    { MODE_MAGIC, 5,  0, false, true,  SELECT_SLOT05, false, 0 },
    { MODE_MAGIC, 6,  0, false, true,  SELECT_SLOT06, false, 0 },
    { MODE_MAGIC, 7,  0, false, true,  SELECT_SLOT07, false, 0 },
    { MODE_MAGIC, 8,  0, false, true,  SELECT_SLOT08, false, 0 },
    { MODE_MAGIC, 9,  0, false, true,  SELECT_SLOT09, false, 0 },
    { MODE_MAGIC, 10, 0, false, true,  SELECT_SLOT10, false, 0 },
    { MODE_MAGIC, 0,  0, true,  false, EXIT_MAGIC_MODE, false, 0 },

    /* -- Slot programming -- */
    { MODE_SLOT01, 0, 0, false, true, SLOT01_CLICKX, false, 0 },
    { MODE_SLOT02, 0, 0, false, true, SLOT02_CLICKX, false, 0 },
    { MODE_SLOT03, 0, 0, false, true, SLOT03_CLICKX, false, 0 },
    { MODE_SLOT04, 0, 0, false, true, SLOT04_CLICKX, false, 0 },
    { MODE_SLOT05, 0, 0, false, true, SLOT05_CLICKX, false, 0 },
    { MODE_SLOT06, 0, 0, false, true, SLOT06_CLICKX, false, 0 },
    { MODE_SLOT07, 0, 0, false, true, SLOT07_CLICKX, false, 0 },
    { MODE_SLOT08, 0, 0, false, true, SLOT08_CLICKX, false, 0 },
    { MODE_SLOT09, 0, 0, false, true, SLOT09_CLICKX, false, 0 },
    { MODE_SLOT10, 0, 0, false, true, SLOT10_CLICKX, false, 0 },

    /* -- Block/unblock -- */
    { MODE_BLOCK,  6, 0, false, false, UNBLOCK_DRV,   false, 0 },
    { MODE_BLOCK0, 6, 0, false, false, UNBLOCK_DRV,   false, 0 },
};

#define USR_TABLE_SIZE (sizeof(usr_table) / sizeof(usr_table[0]))

/*******************************************************************************
 *  UI dispatch lookup
 ******************************************************************************/

uint8_t ui_table_lookup(uint8_t mode, const btn_evt_t *evt, bool timeout)
{
    const dispatch_t *table;
    uint8_t size;

    uint8_t idx = ui_table_idx;

    if (idx == 1)
    {
        table = ui1_table;
        size = UI1_TABLE_SIZE;
    }
    else if (idx == 2 || idx == 3)
    {
        table = ui2_table;
        size = UI2_TABLE_SIZE;
    }
    else if (idx == 5)  // ENG_MENU_IND
    {
        table = eng_table;
        size = ENG_TABLE_SIZE;
    }
    else if (idx == 6)  // USR_MENU_IND
    {
        table = usr_table;
        size = USR_TABLE_SIZE;
    }
    else
    {
        return 0;
    }

    for (uint8_t i = 0; i < size; i++)
    {
        uint8_t f_mode  = table[i].mode;
        uint8_t f_clicks = table[i].clicks;
        uint8_t f_lmask = table[i].long_mask;
        uint8_t f_hold  = table[i].hold;
        uint8_t f_wait  = table[i].wait_timeout;
        uint8_t f_slot  = table[i].is_slot;
        uint8_t f_op    = table[i].op_index;

        if (f_mode != mode) continue;
        if (f_clicks != 0 && f_clicks != evt->clicks) continue;
        if (f_lmask && (evt->long_mask & f_lmask) != f_lmask) continue;
        if (f_hold != (evt->type == BTN_EVT_HOLD)) continue;
        if (f_wait && !timeout) continue;

        uint8_t op_idx = f_op;
#ifdef USE_MAGIC_SLOTS
        if (f_slot)
            op_idx = ob_GetSlot(table[i].slot_idx);
#endif

        if (op_idx < MAIN_OP_COUNT && main_op[op_idx])
        {
            /* Slots are one-shot commands during a hold.  The button FSM
             * observes this existing AVR-compatible flag before emitting
             * further HOLD repeats. */
            if (f_slot)
                off_auto_repeat();
            main_op[op_idx]();
            return 1;
        }
        return 0;
    }

    return 0;
}

/*******************************************************************************
 *  main_op[] -- action function table
 ******************************************************************************/

void (*main_op[])(void) = {
    /*00*/ exit_magic_mode,
    /*01*/ slot_empty,
    /*02*/ slot_batt_ind,
    /*03*/ slot_strobe,
    /*04*/ slot_block_beacon,
    /*05*/ slot_block,
    /*06*/ slot_tactical,
    /*07*/ slot_backlight,
    /*08*/ slot_beacon,
    /*09*/ slot_eauxmode,
    /*10*/ slot_ipsmode,
    /*11*/ slot_memory,

    /*12..20 reserved */
    0, 0, 0, 0, 0, 0, 0, 0, 0,

    /*21*/ up_strobe,
    /*22*/ back_strobe,
    /*23*/ off_strobe,
    /*24*/ unblock_drv,
    /*25*/ off_driver_false,

    /*26*/ on_xmode_ui1,
    /*27*/ on_rumpup_ui1,
    /*28*/ on_rumpdown_ui1,
    /*29*/ on_turbo_ui1,
    /*30*/ calibration,

    /*31*/ enter_rs_mode,
    /*32*/ enter_ml_mode,
    /*33*/ enter_pt_mode,
    /*34*/ save_def_ui1,
    /*35*/ save_def_ui2,

    /*36*/ rs_clickX,
    /*37*/ ml_clickX,
    /*38*/ pt_clickX,

    /*39*/ menu_exit,

    /*40*/ enter_chtk_mode,
    /*41*/ chtk_clickX,

    /*42*/ enter_bm_mode,
    /*43*/ bm_clickX,

    /*44*/ enter_bt_mode,
    /*45*/ bt_clickX,

    /*46*/ enter_bl_mode,
    /*47*/ bl_clickX,

    /*48*/ enter_magic_mode,

    /*49..68*/
    select_slot, slotN_clickX,
    select_slot, slotN_clickX,
    select_slot, slotN_clickX,
    select_slot, slotN_clickX,
    select_slot, slotN_clickX,
    select_slot, slotN_clickX,
    select_slot, slotN_clickX,
    select_slot, slotN_clickX,
    select_slot, slotN_clickX,
    select_slot, slotN_clickX,

    /*69..78*/
    mode_up_ui2,
    on_turbo_ui2,
    on_min_ui2,
    on_moon_ui2,
    on_xmode_ui2,
    mode_down_ui2,
    step_down_ui2,
    enter_uis_mode,
    uis_clickX,
    reset2def,
};
