/*
 * ui_table.c
 *
 * Dispatch tables + main_op[] function pointer array.
 *
 * Ported from Proteus IID (AVR / PROGMEM) to CH32V003 RAM.
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

/* UI1 normally turns the driver off on the first click.  When SLOT_BLOCK is
 * assigned to a click-sequence slot, defer that one-click decision until the
 * sequence timeout so a 3..6 click block command can be recognised. */
static bool block_click_pending;

/**
 * @brief Check whether SLOT_BLOCK uses a click-sequence gesture.
 * @return true when a configured even slot 3..6-click gesture starts block.
 */
static bool has_block_click_slot(void)
{
#ifdef USE_MAGIC_SLOTS
    for (uint8_t slot = 2u; slot < SLOTS_COUNT; slot += 2u)
    {
        if (ob_GetSlot(slot) == SLOT_BLOCK)
            return true;
    }
#endif
    return false;
}

/**
 * @brief Check whether an event is the unlock gesture for a slot.
 * @param slot    Slot index (0..SLOTS_COUNT-1).
 * @param evt     Button event to test.
 * @param timeout True after click-sequence timeout.
 * @return true when the event matches the slot's physical gesture.
 */
static bool slot_gesture_matches(uint8_t slot, const btn_evt_t *evt,
                                 bool timeout)
{
    bool is_hold_slot = (slot & 1u) != 0u || slot == 0u;
    uint8_t req_clicks = (slot <= 1u) ? (uint8_t)(slot + 1u)
                                     : (uint8_t)((slot >> 1) + 2u);

    if (is_hold_slot)
        return evt->type == BTN_EVT_HOLD && evt->clicks == req_clicks;

    return timeout && evt->type == BTN_EVT_CLICK_SEQ &&
           evt->clicks == req_clicks;
}

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
    { MODE_OFF, 1, 0, false, true,  ON_XMODE_UI1 },
    { MODE_OFF, 2, 0, false, true,  ON_TURBO_UI1 },
    { MODE_OFF, 0, 0, true,  false, ON_RUMPUP_UI1 },

    /* -- Mode: MODE_ON -- */
    { MODE_ON,  1, 0, false, true,  OFF_DRV_FALSE },
    { MODE_ON,  1, 0, true,  false, ON_RUMPDOWN_UI1 },
    { MODE_ON,  5, 0, true,  false, SAVE_DEF_UI1 },
    { MODE_ON,  0, 0, true,  false, ON_RUMPUP_UI1 },
    { MODE_ON,  2, 0, false, true,  ON_TURBO_UI1 },

    /* -- Mode: MODE_STROB -- */
    { MODE_STROB, 1, 0, false, true,  UP_STROBE_UI1 },
    { MODE_STROB, 0, 0, true,  false, OFF_STROBE_UI1 },
};

#define UI1_TABLE_SIZE (sizeof(ui1_table) / sizeof(ui1_table[0]))

/*******************************************************************************
 *  UI2 dispatch table (discrete brightness interface)
 ******************************************************************************/

static const dispatch_t ui2_table[] = {
    /* -- Mode: MODE_OFF -- */
    { MODE_OFF, 1, 1, false, false, ON_MIN_UI2 },
    { MODE_OFF, 1, 0, false, true,  ON_XMODE_UI2 },
    { MODE_OFF, 2, 0, false, true,  ON_TURBO_UI2 },
    { MODE_OFF, 0, 0, true,  false, ON_MOON_UI2 },

    /* -- Mode: MODE_ON -- */
    { MODE_ON,  1, 1, false, false, STEP_DOWN_UI2 },
    { MODE_ON,  1, 0, false, true,  OFF_DRV_FALSE },
    { MODE_ON,  1, 0, true,  false, MODE_DOWN_UI2 },
    { MODE_ON,  5, 0, true,  false, SAVE_DEF_UI2 },
    { MODE_ON,  0, 0, true,  false, MODE_UP_UI2 },
    { MODE_ON,  2, 0, false, true,  ON_TURBO_UI2 },

    /* -- Mode: MODE_STROB -- */
    { MODE_STROB, 1, 0, false, true,  UP_STROBE_UI1 },
    { MODE_STROB, 0, 0, true,  false, OFF_STROBE_UI1 },
};

#define UI2_TABLE_SIZE (sizeof(ui2_table) / sizeof(ui2_table[0]))

/*******************************************************************************
 *  Engineering menu dispatch table (ui_index == 5 / ENG_MENU_IND)
 ******************************************************************************/

static const dispatch_t eng_table[] = {
    /* -- First level: MODE_OFF (navigate sub-menus) -- */
    { MODE_OFF, 1,  0, false, true,  ENTER_ML_MODE },
    { MODE_OFF, 5,  0, false, true,  CALIBRATION },
    { MODE_OFF, 15, 0, false, true,  RESET2DEF },
    { MODE_OFF, 0,  0, true,  false, MENU_EXIT },

    /* -- Sub-modes -- */
    { MODE_MOON_LEVEL, 0, 0, false, true,  ML_CLICKX },
};

#define ENG_TABLE_SIZE (sizeof(eng_table) / sizeof(eng_table[0]))

/*******************************************************************************
 *  User settings menu dispatch table (ui_index == 6 / USR_MENU_IND)
 ******************************************************************************/

static const dispatch_t usr_table[] = {
    /* -- First level: MODE_OFF -- */
    { MODE_OFF, 1, 0, false, true,  ENTER_BM_MODE },    /* 1 клик: период маяка (EE.aux.beacon_mode, 1..4) */
    { MODE_OFF, 2, 0, false, true,  ENTER_BT_MODE },    /* 2 клика: цвет маяка  (EE.aux.beacon_type, 1..3) */
    { MODE_OFF, 3, 0, false, true,  ENTER_BL_MODE },    /* 3 клика: цвет подсветки (EE.aux.bcklight_color, 1..2) */
    { MODE_OFF, 4, 0, false, true,  ENTER_UIS_MODE },   /* 4 клика: выбор UI (EE.uif.ui_index, 1..3) */
    { MODE_OFF, 5, 0, false, true,  ENTER_MAGIC_MODE }, /* 5 кликов: настройка magic-слотов (ob_SetSlot, 1..12) */
    { MODE_OFF, 6, 0, false, true,  ENTER_CHTK_MODE },  /* 6 кликов: калибровка температуры (EE.usr.t_calibr, 1..15) */
    { MODE_OFF, 0, 0, true,  false, MENU_EXIT },

    /* -- Sub-modes -- */
    { MODE_BM,  0, 0, false, true,  BM_CLICKX },
    { MODE_BT,  0, 0, false, true,  BT_CLICKX },
    { MODE_BL,  0, 0, false, true,  BL_CLICKX },
    { MODE_UIS, 0, 0, false, true,  UIS_CLICKX },
    { MODE_CTK_H, 0, 0, false, true,  CHTK_CLICKX },

    /* -- Magic mode -- */
    { MODE_MAGIC, 0,  0, true,  false, EXIT_MAGIC_MODE },
};

#define USR_TABLE_SIZE (sizeof(usr_table) / sizeof(usr_table[0]))

/*******************************************************************************
 *  UI dispatch lookup
 ******************************************************************************/

static uint8_t execute_slot(uint8_t slot)
{
    uint8_t op_idx = SLOT_OP_EMPTY;
#ifdef USE_MAGIC_SLOTS
    op_idx = ob_GetSlot(slot);
    if (op_idx < SLOT_OP_FIRST || op_idx > SLOT_OP_LAST)
        op_idx = SLOT_OP_EMPTY;
#endif

    /* While the driver is on, only the plain block operation is allowed.
     * In particular SLOT_BLOCK_BEACON must not replace the normal beacon
     * behaviour or alter the active driver state. */
    if (driver_mode == MODE_ON && op_idx != SLOT_BLOCK)
        return 0;

    if (op_idx < MAIN_OP_COUNT && main_op[op_idx])
    {
        off_auto_repeat();
        main_op[op_idx]();
        if (driver_mode == MODE_BLOCK || driver_mode == MODE_BLOCK0 ||
            driver_mode == MODE_BLOCK_ON)
            block_slot_idx = slot;
        return 1;
    }
    return 0;
}

/**
 * @brief Look up and dispatch a button event in the active UI table.
 * @param mode    Current driver_mode.
 * @param evt     Current button event.
 * @param timeout True if the click-sequence timeout has elapsed.
 * @return 1 if an action was dispatched, 0 if no match found.
 */
uint8_t ui_table_lookup(uint8_t mode, const btn_evt_t *evt, bool timeout)
{
    uint8_t idx = ui_table_idx;

    if (mode == MODE_BLOCK || mode == MODE_BLOCK0 || mode == MODE_BLOCK_ON)
    {
        if (block_slot_idx < SLOTS_COUNT)
        {
            if (slot_gesture_matches(block_slot_idx, evt, timeout))
            {
                off_auto_repeat();
                unblock_drv();
                block_slot_idx = SLOTS_COUNT;
                block_click_pending = false;
                return 1;
            }
        }
        return 0;
    }

    /* UI1's one-click OFF action must wait while a click-sequence slot may
     * contain SLOT_BLOCK.  A later CLICK_SEQ either enters block or replays
     * the ordinary one-click OFF below. */
    if (mode == MODE_ON && idx == 1u && evt->type == BTN_EVT_RELEASED &&
        evt->clicks == 1u && has_block_click_slot())
    {
        block_click_pending = true;
        return 1;
    }

    /* Common slot dispatch in UI1 / UI2 (MODE_OFF and MODE_ON). */
    if ((mode == MODE_OFF || mode == MODE_ON) && (idx >= 1u && idx <= 3u))
    {
        int8_t slot = -1;
        if (evt->type == BTN_EVT_HOLD)
        {
            if (evt->clicks == 1) slot = 0;
            else if (evt->clicks == 2) slot = 1;
            else if (evt->clicks >= 3 && evt->clicks <= 6) slot = (int8_t)((evt->clicks - 2) * 2 + 1);
        }
        else if (timeout && (evt->type == BTN_EVT_CLICK_SEQ))
        {
            if (evt->clicks >= 3 && evt->clicks <= 6) slot = (int8_t)((evt->clicks - 2) * 2);
        }

        if (slot >= 0)
        {
            uint8_t dispatched = execute_slot((uint8_t)slot);
            if (dispatched)
            {
                block_click_pending = false;
                return 1;
            }
        }
    }

    /* Restore UI1's normal one-click OFF semantics if the sequence ended
     * without matching the configured SLOT_BLOCK gesture.  Other sequences
     * must continue to the normal UI table (notably UI1's two-click turbo). */
    if (mode == MODE_ON && idx == 1u && timeout &&
        evt->type == BTN_EVT_CLICK_SEQ && block_click_pending)
    {
        block_click_pending = false;
        if (evt->clicks == 1u)
        {
            off_driver_false();
            return 1;
        }
    }

    /* Sub-modes HOLD -> MENU_EXIT */
    if (evt->type == BTN_EVT_HOLD && ((mode >= MODE_BM && mode <= MODE_UIS) || mode == MODE_CTK_H || mode == MODE_MOON_LEVEL))
    {
        menu_exit();
        return 1;
    }

    /* Magic slot range handling */
    if (timeout && (evt->type == BTN_EVT_CLICK_SEQ))
    {
        if (mode == MODE_MAGIC)
        {
            if (evt->clicks >= 1u && evt->clicks <= SLOTS_COUNT)
            {
                select_slot();
                return 1;
            }
            ui_invalid_selection();
            return 0;
        }

        if (mode >= MODE_SLOT01 && mode <= MODE_SLOT10)
        {
            slotN_clickX();
            return 1;
        }
    }

    const dispatch_t *table;
    uint8_t size;

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
        const dispatch_t *entry = &table[i];
        if (!dispatch_matches(entry, mode, evt, timeout)) continue;

        uint8_t op_idx = entry->op_index;
        if (op_idx < MAIN_OP_COUNT && main_op[op_idx])
        {
            main_op[op_idx]();
            return 1;
        }
        return 0;
    }

    return 0;
}

/*******************************************************************************
 *  main_op[] -- action function table (const → flash, не занимает RAM)
 ******************************************************************************/

void (* const main_op[])(void) = {
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

    /*12*/ slot_ui_select,

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

    /*23*/ enter_ml_mode,
    /*24*/ save_def_ui1,
    /*25*/ save_def_ui2,

    /*26*/ ml_clickX,

    /*27*/ menu_exit,

    /*28*/ enter_chtk_mode,
    /*29*/ chtk_clickX,

    /*30*/ enter_bm_mode,
    /*31*/ bm_clickX,

    /*32*/ enter_bt_mode,
    /*33*/ bt_clickX,

    /*34*/ enter_bl_mode,
    /*35*/ bl_clickX,

    /*36*/ enter_magic_mode,

    /*37..38*/
    select_slot, slotN_clickX,

    /*39..48*/
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
