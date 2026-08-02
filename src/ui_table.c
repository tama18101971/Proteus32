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

#define MAIN_OP_COUNT (RESET2DEF + 1)

/* Current UI index used for table selection. Defined in ui.c. */
extern uint8_t ui_table_idx;

/* The slot which entered MODE_BLOCK/MODE_BLOCK0.  Standby preserves RAM, so
 * the same gesture can unlock after a button wake without a flash write. */
static uint8_t block_slot_idx = SLOTS_COUNT;

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

/**
 * @brief Check whether a dispatch table entry matches the current event.
 * @param entry  Table entry to test.
 * @param mode   Current driver_mode.
 * @param evt    Current button event.
 * @param timeout True if the click-sequence timeout has elapsed.
 * @return true if all fields match (mode, clicks, long_mask, hold, timeout).
 */
static bool dispatch_matches(const dispatch_t *entry, uint8_t mode,
                              const btn_evt_t *evt, bool timeout)
{
    if (entry->mode != mode) return false;
    if (entry->clicks != 0 && entry->clicks != evt->clicks) return false;
    if (entry->long_mask
        && (evt->long_mask & entry->long_mask) != entry->long_mask) return false;
    if (entry->hold != (evt->type == BTN_EVT_HOLD)) return false;
    if (entry->wait_timeout && !timeout) return false;

    return true;
}

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
    { MODE_MAGIC, 1,  0, false, true,  SELECT_SLOT, false, 0 },
    { MODE_MAGIC, 2,  0, false, true,  SELECT_SLOT, false, 0 },
    { MODE_MAGIC, 3,  0, false, true,  SELECT_SLOT, false, 0 },
    { MODE_MAGIC, 4,  0, false, true,  SELECT_SLOT, false, 0 },
    { MODE_MAGIC, 5,  0, false, true,  SELECT_SLOT, false, 0 },
    { MODE_MAGIC, 6,  0, false, true,  SELECT_SLOT, false, 0 },
    { MODE_MAGIC, 7,  0, false, true,  SELECT_SLOT, false, 0 },
    { MODE_MAGIC, 8,  0, false, true,  SELECT_SLOT, false, 0 },
    { MODE_MAGIC, 9,  0, false, true,  SELECT_SLOT, false, 0 },
    { MODE_MAGIC, 10, 0, false, true,  SELECT_SLOT, false, 0 },
    { MODE_MAGIC, 0,  0, true,  false, EXIT_MAGIC_MODE, false, 0 },

    /* -- Slot programming -- */
    { MODE_SLOT01, 0, 0, false, true, SLOT_CLICKX, false, 0 },
    { MODE_SLOT02, 0, 0, false, true, SLOT_CLICKX, false, 0 },
    { MODE_SLOT03, 0, 0, false, true, SLOT_CLICKX, false, 0 },
    { MODE_SLOT04, 0, 0, false, true, SLOT_CLICKX, false, 0 },
    { MODE_SLOT05, 0, 0, false, true, SLOT_CLICKX, false, 0 },
    { MODE_SLOT06, 0, 0, false, true, SLOT_CLICKX, false, 0 },
    { MODE_SLOT07, 0, 0, false, true, SLOT_CLICKX, false, 0 },
    { MODE_SLOT08, 0, 0, false, true, SLOT_CLICKX, false, 0 },
    { MODE_SLOT09, 0, 0, false, true, SLOT_CLICKX, false, 0 },
    { MODE_SLOT10, 0, 0, false, true, SLOT_CLICKX, false, 0 },

};

#define USR_TABLE_SIZE (sizeof(usr_table) / sizeof(usr_table[0]))

/*******************************************************************************
 *  UI dispatch lookup
 ******************************************************************************/

/**
 * @brief Look up and dispatch a button event in the active UI table.
 * @param mode    Current driver_mode.
 * @param evt     Current button event.
 * @param timeout True if the click-sequence timeout has elapsed.
 * @return 1 if an action was dispatched, 0 if no match found.
 */
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

    if (mode == MODE_BLOCK || mode == MODE_BLOCK0)
    {
        if (block_slot_idx >= SLOTS_COUNT)
            return 0;

        for (uint8_t i = 0; i < size; i++)
        {
            const dispatch_t *entry = &table[i];

            /* The OFF-state slot definition is the canonical gesture.  It
             * retains both the click count and whether the action fires on
             * hold or only after the click-sequence timeout. */
            if (entry->mode != MODE_OFF || !entry->is_slot
                || entry->slot_idx != block_slot_idx) continue;
            if (!dispatch_matches(entry, MODE_OFF, evt, timeout)) continue;

            off_auto_repeat();
            unblock_drv();
            block_slot_idx = SLOTS_COUNT;
            return 1;
        }

        return 0;
    }

    for (uint8_t i = 0; i < size; i++)
    {
        const dispatch_t *entry = &table[i];
        uint8_t f_slot = entry->is_slot;
        uint8_t f_op = entry->op_index;

        if (!dispatch_matches(entry, mode, evt, timeout)) continue;

        uint8_t op_idx = f_op;
#ifdef USE_MAGIC_SLOTS
        if (f_slot)
        {
            op_idx = ob_GetSlot(entry->slot_idx);
            if (op_idx < SLOT_OP_FIRST || op_idx > SLOT_OP_LAST)
                op_idx = SLOT_OP_EMPTY;
        }
#endif

        if (op_idx < MAIN_OP_COUNT && main_op[op_idx])
        {
            /* Slots are one-shot commands during a hold.  The button FSM
             * observes this existing AVR-compatible flag before emitting
             * further HOLD repeats. */
            if (f_slot)
                off_auto_repeat();
            main_op[op_idx]();
            if (f_slot && (driver_mode == MODE_BLOCK || driver_mode == MODE_BLOCK0))
                block_slot_idx = entry->slot_idx;
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

    /*12 reserved */
    0,

    /*13*/ up_strobe,
    /*14*/ back_strobe,
    /*15*/ off_strobe,
    /*16*/ unblock_drv,
    /*17*/ off_driver_false,

    /*18*/ on_xmode_ui1,
    /*19*/ on_rumpup_ui1,
    /*20*/ on_rumpdown_ui1,
    /*21*/ on_turbo_ui1,
    /*22*/ calibration,

    /*23*/ enter_rs_mode,
    /*24*/ enter_ml_mode,
    /*25*/ enter_pt_mode,
    /*26*/ save_def_ui1,
    /*27*/ save_def_ui2,

    /*28*/ rs_clickX,
    /*29*/ ml_clickX,
    /*30*/ pt_clickX,

    /*31*/ menu_exit,

    /*32*/ enter_chtk_mode,
    /*33*/ chtk_clickX,

    /*34*/ enter_bm_mode,
    /*35*/ bm_clickX,

    /*36*/ enter_bt_mode,
    /*37*/ bt_clickX,

    /*38*/ enter_bl_mode,
    /*39*/ bl_clickX,

    /*40*/ enter_magic_mode,

    /*41..42*/
    select_slot, slotN_clickX,

    /*43..52*/
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
