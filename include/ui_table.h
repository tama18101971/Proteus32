// SPDX-License-Identifier: GPL-3.0-or-later

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
    uint8_t op_index   : 6;  // action function index in main_op[]
} dispatch_t;

_Static_assert(sizeof(dispatch_t) == 4,
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

/*
 *  Индексы 0..12 заморожены: 0..11 — содержимое magic-слотов (FUN_Index_t,
 *  persist-ятся в EE нибблами), 12 — SLOT_UI_SELECT (верхняя граница
 *  SLOT_OP_LAST).  Всё, что выше, — внутренние номера main_op[] и может быть
 *  перенумеровано свободно: недостижимые и дублирующиеся записи удалены,
 *  индексы идут подряд (проверяется _Static_assert в ui_table.c).
 */

/* -- UI1 strobe / block (13..16) -- */
#define UP_STROBE_UI1       13
#define OFF_STROBE_UI1      14
#define UNBLOCK_DRV         15
#define OFF_DRV_FALSE       16

/* -- UI1 brightness / mode (17..21) -- */
#define ON_XMODE_UI1        17
#define ON_RUMPUP_UI1       18
#define ON_RUMPDOWN_UI1     19
#define ON_TURBO_UI1        20
#define CALIBRATION         21

/* -- Engineering menu: enter (22..24) -- */
#define ENTER_ML_MODE       22
#define SAVE_DEF_UI1        23
#define SAVE_DEF_UI2        24

/* -- Menu exit (25).  Подрежимы ввода кликов (menu_clickX) вызываются из
 *    ui_table_lookup напрямую и указателей main_op[] не имеют. -- */
#define MENU_EXIT           25

/* -- User menu: enter (26..30) -- */
#define ENTER_CHTK_MODE     26
#define ENTER_BM_MODE       27
#define ENTER_BT_MODE       28
#define ENTER_BL_MODE       29
#define ENTER_MAGIC_MODE    30

/* -- UI2 actions (31..39) -- */
#define MODE_UP_UI2         31
#define ON_TURBO_UI2        32
#define ON_MIN_UI2          33
#define ON_MOON_UI2         34
#define ON_XMODE_UI2        35
#define MODE_DOWN_UI2       36
#define STEP_DOWN_UI2       37
#define ENTER_UIS_MODE      38
#define RESET2DEF           39

/* -- User menu: lockout min-light (40..41) -- */
#define ENTER_LM_EN_MODE    40
#define ENTER_LM_TIME_MODE  41

#if USE_EX_AUX
/* -- User menu: extended aux voltage warning (42) -- */
#define ENTER_EAUX_MODE     42
#endif

/* -- Верхняя граница (не включительно) для dispatch_t::op_index -- */
#if USE_EX_AUX
#define MAIN_OP_LAST        43
#else
#define MAIN_OP_LAST        42
#endif

/*******************************************************************************
 *  Main action function table
 ******************************************************************************/

extern void (* const main_op[])(void);

#endif /* UI_TABLE_H_ */
