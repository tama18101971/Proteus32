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

#define MAIN_OP_COUNT MAIN_OP_LAST

/* Current UI index used for table selection. Defined in ui.c. */
extern uint8_t ui_table_idx;

/* ui_table_lookup отсекает режимы блокировки проверкой принадлежности к
 * BLOCK_MODE_MASK.  Обычные режимы (MODE_BLOCK_C, MODE_OFF, MODE_ON,
 * MODE_STROB) живут в том же числовом диапазоне 0..MODE_BLOCK_LIGHT, поэтому
 * множество задаётся маской, а не сравнением «mode <= MODE_BLOCK_LIGHT».
 *
 * Верхняя граница 0..31 — условие корректности самой маски. */
#define BLOCK_MODE_MASK ((1u << MODE_BLOCK) | (1u << MODE_BLOCK0) | \
                         (1u << MODE_BLOCK_ON) | (1u << MODE_BLOCK_LIGHT))

_Static_assert((MODE_BLOCK <= MODE_BLOCK_LIGHT) && (MODE_BLOCK0 <= MODE_BLOCK_LIGHT) &&
               (MODE_BLOCK_ON <= MODE_BLOCK_LIGHT) && (MODE_BLOCK_LIGHT <= 31),
               "all lockout modes must fit in 0..31 for BLOCK_MODE_MASK to work");

/* has_block_click_slot() жёстко перебирает байты 1..4 slots_packed — младшие
 * нибблы слотов 2,4,6,8.  Раскладка верна ровно при 10 слотах по 4 бита. */
_Static_assert(SLOTS_COUNT == 10 && sizeof(((app_cfg_t *)0)->slots_packed) == 5,
               "has_block_click_slot() nibble layout assumes 10 slots x 4 bits");

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
#if USE_MAGIC_SLOTS
    /* Слоты 2,4,6,8 — это младшие нибблы байтов 1..4 массива slots_packed,
     * поэтому проверяются без вызова slot_get() (по одному чтению на байт). */
    for (uint8_t byte = 1u; byte < 5u; byte++)
    {
        if ((EE.slots_packed[byte] & 0x0Fu) == SLOT_BLOCK)
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
    { MODE_STROB, 1, 0, false, true,  OFF_STROBE_UI1 },
    { MODE_STROB, 0, 0, true,  false, UP_STROBE_UI1 },
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
    { MODE_STROB, 1, 0, false, true,  OFF_STROBE_UI1 },
    { MODE_STROB, 0, 0, true,  false, UP_STROBE_UI1 },
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
    /* Подрежим MODE_MOON_LEVEL обрабатывается общей проверкой в
     * ui_table_lookup() (см. комментарий у usr_table). */
};

#define ENG_TABLE_SIZE (sizeof(eng_table) / sizeof(eng_table[0]))

/*******************************************************************************
 *  User settings menu dispatch table (ui_index == 6 / USR_MENU_IND)
 ******************************************************************************/

static const dispatch_t usr_table[] = {
    /* -- First level: MODE_OFF -- */
    { MODE_OFF, 1, 0, false, true,  ENTER_BM_MODE },        /* 1 клик: период маяка (EE.aux.beacon_mode, 1..4) */
    { MODE_OFF, 2, 0, false, true,  ENTER_BT_MODE },        /* 2 клика: цвет маяка  (EE.aux.beacon_type, 1..3; RGB: 1..4) */
    { MODE_OFF, 3, 0, false, true,  ENTER_BL_MODE },        /* 3 клика: цвет подсветки (EE.aux.bcklight_color, 1..2; RGB: 1..3) */
    { MODE_OFF, 4, 0, false, true,  ENTER_UIS_MODE },       /* 4 клика: выбор UI (EE.uif.ui_index, 1..3) */
    { MODE_OFF, 5, 0, false, true,  ENTER_MAGIC_MODE },     /* 5 кликов: настройка magic-слотов (slot_set, 1..12) */
    { MODE_OFF, 6, 0, false, true,  ENTER_LM_EN_MODE },     /* 6 кликов: вкл/выкл подсветки блокировки (1..2) */
    { MODE_OFF, 7, 0, false, true,  ENTER_LM_TIME_MODE },   /* 7 кликов: время подсветки блокировки (1..4) */
    { MODE_OFF, 8, 0, false, true,  ENTER_CHTK_MODE },      /* 8 кликов: калибровка температуры (EE.usr.t_calibr, 1..15) */
#if USE_EX_AUX
    { MODE_OFF, 9, 0, false, true,  ENTER_EAUX_MODE },      /* 9 кликов: ext aux voltage warning (EE.uif.eaux_onoff, 1..2) */
#endif
    { MODE_OFF, 0, 0, true,  false, MENU_EXIT },

    /* -- Magic mode -- */
    { MODE_MAGIC, 0, 0, true,  false, EXIT_MAGIC_MODE },
};

/* Подрежимы ввода кликов (MODE_MOON_LEVEL, MODE_CTK_H, MODE_UIS..MODE_LM_TIME,
 * MODE_EAUX) разбирает ui_table_lookup() напрямую вызовом menu_clickX() —
 * девять одинаковых строк «mode + любые клики» и девять указателей
 * main_op[] на один и тот же menu_clickX были чистым дублем. */

#define USR_TABLE_SIZE (sizeof(usr_table) / sizeof(usr_table[0]))

/* Дескрипторы таблиц диспетчеризации по ui_table_idx: заменяют каскад
 * if/else-if в ui_table_lookup одним индексированным доступом.
 * Индексы: 1..3 — интерфейсы (UI2 и UI3 шарят одну таблицу), 5/6 — меню
 * (ENG_MENU_IND/USR_MENU_IND; назначаются только при загрузке из
 * enter_menu_at_boot). Ячейки [0] и [4] пусты — такие idx отсекаются
 * проверкой в ui_table_lookup. */
/* Deskriptor razdeljon na dva massiva: ui_table_desc_t (ukazatel + uint8 +
 * vyravnivanie) zanimala 56 B rodata, dva massiva - 35 B.
 * Interfejsy i razmery indeksirujutsja odnim i tem zhe ui_table_idx. */
static const dispatch_t *const ui_tab_ptr[7] = {
    [1] = ui1_table,
    [2] = ui2_table,
    [3] = ui2_table,
    [5] = eng_table,
    [6] = usr_table,
};

static const uint8_t ui_tab_size[7] = {
    [1] = UI1_TABLE_SIZE,
    [2] = UI2_TABLE_SIZE,
    [3] = UI2_TABLE_SIZE,
    [5] = ENG_TABLE_SIZE,
    [6] = USR_TABLE_SIZE,
};

/*******************************************************************************
 *  UI dispatch lookup
 ******************************************************************************/

static uint8_t execute_slot(uint8_t slot)
{
#if USE_MAGIC_SLOTS
    uint8_t op_idx = slot_get(slot);
    if (op_idx < SLOT_OP_FIRST || op_idx > SLOT_OP_LAST)
        op_idx = SLOT_OP_EMPTY;

    /* While the driver is on, only the plain block operation is allowed.
     * In particular SLOT_BLOCK_BEACON must not replace the normal beacon
     * behaviour or alter the active driver state. */
    if (driver_mode == MODE_ON && op_idx != SLOT_BLOCK)
        return 0;

    if (op_idx < MAIN_OP_COUNT)
    {
        off_auto_repeat();
        main_op[op_idx]();
        /* Блокирующие режимы разбросаны по enum (0, 1, 6), диапазон не подходит. */
        if (driver_mode == MODE_BLOCK || driver_mode == MODE_BLOCK0 ||
            driver_mode == MODE_BLOCK_ON)
            block_slot_idx = slot;
        return 1;
    }
#else
    (void)slot; /* Without magic slots the gesture belongs to the table
                 * lookup below — falling through with SLOT_OP_EMPTY would
                 * swallow 1-click-hold ramp, save-def and even-click
                 * sequences before they reach their dispatch rows. */
#endif
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

    if (mode <= MODE_BLOCK_LIGHT && ((1u << mode) & BLOCK_MODE_MASK))
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
        /* Общий жест «1 клик после таймаута последовательности» для всех
         * блок-режимов. MODE_BLOCK_LIGHT: погасить временный свет. Условие
         * mode != MODE_BLOCK_ON обязательно: в этом режиме жест не должен
         * включать подсветку (MODE_BLOCK_ON не попадал ни в одну ветку
         * исходного каскада). */
        if (timeout && evt->type == BTN_EVT_CLICK_SEQ && evt->clicks == 1u)
        {
            if (mode == MODE_BLOCK_LIGHT)
            {
                lock_light_off();
                return 1;
            }
            if (mode != MODE_BLOCK_ON && USRF_GET_LOCK_MIN_EN(EE.usr))
            {
                lock_light_on((DRIVER_Mode_t)mode);
                return 1;
            }
        }
        return 0;
    }

    /* UI1's one-click OFF action must wait while a click-sequence slot may
     * contain SLOT_BLOCK.  A later CLICK_SEQ either enters block or replays
     * the ordinary one-click OFF below.  RELEASED и CLICK_SEQ — непере-
     * секающиеся типы событий, поэтому обе проверки в одном блоке перед
     * slot-dispatch эквивалентны прежнему разнесению (порядок относительно
     * slot-dispatch не менял исход). */
    if (mode == MODE_ON && idx == 1u)
    {
        if (evt->type == BTN_EVT_RELEASED && evt->clicks == 1u && has_block_click_slot())
        {
            block_click_pending = true;
            return 1;
        }
        if (timeout && evt->type == BTN_EVT_CLICK_SEQ && block_click_pending)
        {
            block_click_pending = false;
            if (evt->clicks == 1u)
            {
                off_driver_false();
                return 1;
            }
        }
    }

    /* Common slot dispatch in UI1 / UI2 (MODE_OFF and MODE_ON). */
    if ((mode == MODE_OFF || mode == MODE_ON) && (idx >= 1u && idx <= 3u))
    {
        int8_t slot = -1;
        if (evt->type == BTN_EVT_HOLD)
        {
            if (evt->clicks == 1) slot = 0;
            else if (evt->clicks == 2) slot = 1;
            else if (evt->clicks >= 3 && evt->clicks <= 6) slot = (int8_t)(((evt->clicks - 2) << 1) + 1);
        }
        else if (timeout && (evt->type == BTN_EVT_CLICK_SEQ))
        {
            if (evt->clicks >= 3 && evt->clicks <= 6) slot = (int8_t)((evt->clicks - 2) << 1);
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

    /* Sub-modes HOLD -> MENU_EXIT (MODE_EAUX sits after the contiguous
     * UIS..LM_TIME range and needs its own check). */
    if (evt->type == BTN_EVT_HOLD &&
        ((mode >= MODE_UIS && mode <= MODE_LM_TIME) ||
         mode == MODE_CTK_H || mode == MODE_MOON_LEVEL
#if USE_EX_AUX
         || mode == MODE_EAUX
#endif
        ))
    {
        menu_exit();
        return 1;
    }

    /* Подрежимы ввода кликов: заменяют девять одинаковых строк «mode + любые
     * клики» в eng_table/usr_table. Диапазон MODE_UIS..MODE_LM_TIME
     * непрерывен; MODE_MOON_LEVEL, MODE_CTK_H и (USE_EX_AUX) MODE_EAUX стоят
     * вне его, MODE_MAGIC/MODE_CTK_A ввода кликов не имеют. Условие
     * повторяет бывшую строку таблицы: clicks/long_mask — любые, hold=false,
     * wait_timeout=true. */
    if (timeout && evt->type != BTN_EVT_HOLD
        && (mode == MODE_MOON_LEVEL || mode == MODE_CTK_H
            || (mode >= MODE_UIS && mode <= MODE_LM_TIME)
#if USE_EX_AUX
            || mode == MODE_EAUX
#endif
           ))
    {
        menu_clickX();
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

    if (idx > 6u || ui_tab_ptr[idx] == NULL)
        return 0;

    const dispatch_t *table = ui_tab_ptr[idx];
    const uint8_t size = ui_tab_size[idx];

    for (uint8_t i = 0; i < size; i++)
    {
        const dispatch_t *entry = &table[i];
        if (!dispatch_matches(entry, mode, evt, timeout)) continue;

        uint8_t op_idx = entry->op_index;
        if (op_idx < MAIN_OP_COUNT)
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
    /*14*/ off_strobe,
    /*15*/ unblock_drv,
    /*16*/ off_driver_false,

    /*17*/ on_xmode_ui1,
    /*18*/ on_rumpup_ui1,
    /*19*/ on_rumpdown_ui1,
    /*20*/ on_turbo_ui1,
    /*21*/ calibration,

    /*22*/ enter_ml_mode,
    /*23*/ save_def_ui1,
    /*24*/ save_def_ui2,

    /*25*/ menu_exit,

    /*26*/ enter_chtk_mode,

    /*27*/ enter_bm_mode,
    /*28*/ enter_bt_mode,
    /*29*/ enter_bl_mode,

    /*30*/ enter_magic_mode,

    /*31*/ mode_up_ui2,
    /*32*/ on_turbo_ui2,
    /*33*/ on_min_ui2,
    /*34*/ on_moon_ui2,
    /*35*/ on_xmode_ui2,
    /*36*/ mode_down_ui2,
    /*37*/ step_down_ui2,
    /*38*/ enter_uis_mode,
    /*39*/ reset2def,

    /*40*/ enter_lm_en_mode,
    /*41*/ enter_lm_time_mode,
#if USE_EX_AUX
    /*42*/ enter_eaux_mode,
#endif
};

/* Удалённые записи: бывший back_strobe (недостижим из таблиц), select_slot() и
 * slotN_clickX() (вызываются напрямую из ui_table_lookup), а также девять
 * дублирующихся указателей на menu_clickX (подрежимы меню теперь разбирает
 * ui_table_lookup напрямую). Индексы идут подряд — иначе op_index в таблицах
 * разъедутся молча. */
_Static_assert(sizeof(main_op) / sizeof(main_op[0]) == MAIN_OP_LAST,
               "main_op[] size must match MAIN_OP_LAST (ui_table.h)");

