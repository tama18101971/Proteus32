/**
 * @file ui.c
 * @brief UI action functions + ui_process dispatcher
 *
 * Ported from Proteus IID (AVR) to Proteus003 (CH32V003).
 * Low-level brightness/DAC functions are kept from the original Proteus003 ui.c.
 *
 * AUX LED calls and EEPROM writes are intentionally left as comments,
 * replaced by EE_PRINTF debug output, per integration requirements.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ui.h"
#include "ui_table.h"
#include "array.h"
#include "aux_leds.h"
#include "ob.h"
#include "adc.h"
#include "app_data.h"
#include "dac.h"
#include "timers.h"
#include "limiters.h"
#include "main.h"
#include "led_gpio.h"

#ifndef MIN
#define MIN(a, b)   (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)   (((a) > (b)) ? (a) : (b))
#endif

/*******************************************************************************
 *  Global state
 ******************************************************************************/

uint8_t mode_default = DEFAULT_MODE;
uint8_t mode_current = CMODE_OFF;

DRIVER_Mode_t driver_mode = MODE_OFF;
bool stop_bit = false;

uint16_t step_tc;
uint16_t step_user;
uint16_t max_step = MAX_STEP;
uint16_t step_default = DEFAULT_STEP;

RUMP_Dir_t direction = DIR_UP;

PSTEP_t ps;

uint8_t need_save = false;

uint8_t clicks_count = 0;
uint8_t ui_table_idx = 1;        // runtime selector for dispatch tables
uint8_t ui_save = 0;
bool menu_beacon_mute = false;   // session-only AUX beacon mute while in menu
uint8_t bcklight_type_save = 0;
uint8_t strobe_index = 0;

static uint8_t bt_r_time = 0;    // direction timeout counter
static bool ui1_was_pressed;
static bool ui_select_from_slot;
static bool hold_guard;          // suppress HOLD repeats until the button is released
static uint8_t ui_cfg_idx = 0xFF; // last ui_table_idx the button timing was applied for
#ifdef USE_BLOCK
static DRIVER_Mode_t saved_block_mode = MODE_BLOCK;
#endif
static uint16_t lock_light_ticks = 0;

/*******************************************************************************
 *  UI2 discrete brightness table -- scaled to Proteus003 9-bit range (0..511)
 ******************************************************************************/

static const uint16_t modes[MODES_QTY] = {0, 20, 74, 136, 220, 360, 511};

/**
 * @brief Check whether the main LED driver is physically active.
 * @return true in normal ON mode and in the button-locked ON mode.
 */
bool driver_is_active(void)
{
    return driver_mode == MODE_ON || driver_mode == MODE_BLOCK_ON || driver_mode == MODE_BLOCK_LIGHT;
}

/*******************************************************************************
 *  Power / DAC helpers (Proteus003)
 ******************************************************************************/

/** @brief Power-on sequence: reset limiters, enable DAC, init I2C. */
void ON_POWER(void)
{
    //EE_PRINTF("[UI] ON PWR.\r\n");
    reset_limiters();
    dac_pwrpin_on();
    i2c_init(400000);
    wait_ms(5);  // DAC7571 power-on reset / wake-up
}

/** @brief Power-off sequence: deinit I2C, disable DAC. */
void OFF_POWER(void)
{
    //EE_PRINTF("[UI] OFF PWR.\r\n");
    i2c_deinit();
    dac_pwrpin_off();
}

/** @brief Set EEPROM defaults for all application parameters. */
void ee_defaults(void)
{
    EE.uif.ui_index = 1;
    EE.uif.use_memory = 0;

    EE.aux.bcklight_color = BCK_COLOR_RED;
    EE.aux.bcklight_type = BCK_TYPE_ON;

    EE.aux.beacon_mode = BCON_1S;
    EE.aux.beacon_on = true;
    EE.aux.beacon_type = GRN_LED;

    EE.usr.id_plus_sid = false;
    EE.usr.lock_min_en = 1;
    EE.usr.lock_min_time = 0;
    EE.usr.t_calibr = TCALIBR_DEFAULT;

    EE.default_ui1 = DEFAULT_STEP;
    EE.default_ui2 = DEFAULT_MODE;

    EE.uif.off_bckl       = true;         
}

/**
 * @brief Clamp brightness target to thermal ceiling.
 * @param step Requested brightness step.
 * @return max(step - limiter, 1) when step > 0, or 0 when off/fault.
 */
uint16_t get_step_tc(uint16_t step)
{
    if (thermal_fault_active() || !step)
        return 0u;

    if ((ui_table_idx == 2U || ui_table_idx == 3U) && (step <= modes[CMODE_MID]))
        return step;

    return (step > limiter) ? (uint16_t)(step - limiter) : 1u;
}

/**
 * @brief Get minimum brightness floor.
 * @param step If non-zero, returns moon level offset; otherwise 0.
 * @return ml + BASE_MOON_LEVEL or 0.
 */
uint16_t get_min_level(uint16_t step)
{
    uint16_t ret = step ? (ob_GetMl() + BASE_MOON_LEVEL) : 0;
//    EE_PRINTF("[UI] min_level: ml=%u step=%u -> ret=%u\r\n",
//              (unsigned)ob_GetMl(), (unsigned)step, (unsigned)ret);
    return ret;
}

/**
 * @brief Convert 9-bit brightness step to 12-bit DAC value with gamma correction.
 * @param val 9-bit step (0-511).
 * @return 12-bit DAC value (0-4095).
 */
uint16_t getBright_CRT_9to12Bit(uint16_t val)
{
    if (val < 2) return val;
    if (val < 10) return 1;
    if (val >= MAX_STEP) return DAC_MAX_VALUE;

    uint32_t x = val;
    uint32_t result = (x * x * x) >> 15;

    if (result == 0) result = 1;

    return (uint16_t)result;
}

/** @brief Set DAC output to gamma-corrected + moon level value for given step. */
void out_set_stc(uint16_t step)
{
    uint16_t out = getBright_CRT_9to12Bit(step) + get_min_level(step);
    if (out > DAC_MAX_VALUE) out = DAC_MAX_VALUE;
    dac7571_set_voltage(out);
}

/**
 * @brief Turn on driver (power, direction, vwarning).
 * @return 1 if power was applied, 0 if already on or blocked.
 */
uint8_t on_driver(void)
{
    if (driver_mode < MODE_BLOCK_C)
    {
        return 0;
    }

    if (driver_mode < MODE_ON)
    {
        EE_PRINTF("[UI] ON DRV.\r\n");
        if (aux_needs_dac())
            off_aux_cmd(false);
        ON_POWER();
        down_count = 5;
        vwarning = VWARNING_NONE;
        vwarning_old = VWARNING_NONE;
        direction = DIR_UP;
        driver_mode = MODE_ON;
        return 1;
    }

    return 0;
}

/**
 * @brief Turn off driver with optional soft-start ramp-down.
 * @param reset_step If true, clear step_tc.
 * @param ss_speed   Soft-start speed for ramp-down.
 */
void off_driver(uint8_t reset_step, SoftStartSpeed_t ss_speed)
{
    EE_PRINTF("[UI] OFF DRV.\r\n");

    driver_mode = MODE_OFF;
    direction = DIR_UP;

    set_current(0, ss_speed);
    if ((ss_speed != SS_NO) && !step_tc)
        OFF_POWER();

    reset_limiters();

    if (ps.use_prev)
    {
        step_tc = 0;
        ps.use_prev = false;
        if (!ps.start)
        {
            if (ui_table_idx == 1)
                step_default = ps.pr_step;
            else
                mode_default = ps.pr_step;
        }
    }

    stop_aux_if(IDM_VWARNING, false);
    stop_aux_if(IDM_BEACON, false);

    /* Memory mode: сохранить последний шаг/режим. Реальную запись решает
     * сравнение с тенью на 5-сек тике (main.c). */
    if (EE.uif.use_memory)
    {
        EE.default_ui1 = step_default;
        EE.default_ui2 = mode_default;
    }

    off_auto_repeat();

    if (reset_step)
        step_tc = 0;
}

/**
 * @brief Low-level brightness set (bypasses step_user).
 * @param step_target Target step value.
 * @param ss_speed    Soft-start speed (SS_NO for instant).
 */
void set_current_ll(uint16_t step_target, SoftStartSpeed_t ss_speed)
{
    if (!step_target && step_tc && !step_user)
        step_default = step_tc;

    if (ss_speed != SS_NO)
        ss_init(step_target, step_tc, ss_speed);
    else
    {
        ss_cancel();
        out_set_stc(step_target);
        step_tc = step_target;
    }
}

/**
 * @brief Set brightness with power management.
 * @param step_target Target step value.
 * @param ss_speed    Soft-start speed.
 */
void set_current(uint16_t step_target, SoftStartSpeed_t ss_speed)
{
    step_user = step_target;
    set_current_ll(get_step_tc(step_target), ss_speed);
    if ((!step_target) && (!ss_speed)) OFF_POWER();
}

/** @brief Cancel auto-repeat (stop_bit) if button is held. */
void off_auto_repeat(void)
{
    if (Pressed)
        stop_bit = true;
}

/** @brief Increment brightness by one step (UI1 ramping). */
void step_up_ui1(void)
{
    if (step_user < max_step)
        set_current(++step_user, SS_NO);
}

/** @brief Decrement brightness by one step (UI1 ramping). */
void step_down_ui1(void)
{
    if (step_user > 1)
        set_current(--step_user, SS_NO);
}

/** @brief Ramp up/down one step from hold (UI1). */
void on_rumpup_ui1(void)
{
    on_driver();
    ps.use_prev = false;
    if (direction)
        step_up_ui1();
    else
        step_down_ui1();
}

/** @brief Ramp down one step from hold (UI1). */
void on_rumpdown_ui1(void)
{
    ps.use_prev = false;
    step_down_ui1();
}

/** @brief Go to memorised/default brightness (UI1 click). */
void on_xmode_ui1(void)
{
    ps.use_prev = false;
    on_driver();
    set_current(EE.uif.use_memory ? (step_user ? step_user : step_default) : EE.default_ui1, SS_FAST);
}

/** @brief Toggle turbo mode (max brightness) with prev-step recall (UI1). */
void on_turbo_ui1(void)
{
    uint8_t start = on_driver();

    if (!ps.use_prev)
    {
        ps.start = start;
        ps.pr_step = step_user ? step_user : step_default;
        ps.use_prev = true;
        set_current(max_step, SS_FAST);
    }
    else
    {
        ps.use_prev = false;
        if (ps.start)
            off_driver(true, SS_FAST);
        else
        {
            direction = DIR_UP;
            set_current(ps.pr_step, SS_FAST);
        }
        step_default = ps.pr_step;
    }
}

/** @brief Force off (no ramp-down, used for mode switch). */
void off_driver_false(void)
{
//    EE_PRINTF("[UI] OFF DRV false\r\n");
    driver_mode = MODE_OFF;
    mode_default = mode_current;
    mode_current = CMODE_OFF;
    off_driver(false, SS_FAST);
}

/*******************************************************************************
 *  AUX indicator helper 
 ******************************************************************************/

/**
 * @brief Set AUX indicator LED to green or red (single flash).
 * @param ind_clr IND_GRN for green, IND_RED for red.
 */
static __attribute__((noinline)) void clr_indicator(ind_clr_t ind_clr)  // бывшая onoff_indicator
{
    off_auto_repeat();
    if (ind_clr == IND_GRN)
        aux_on();   // green
    else
        aux_off();  // red
}

/*******************************************************************************
 *  Slot action functions
 ******************************************************************************/

/** @brief Enter tactical momentary mode (slot). */
void slot_tactical(void)
{
    ui_table_idx = 0;
    clr_indicator(IND_GRN);
}

/** @brief Toggle backlight type (slot). */
void slot_backlight(void)
{
    EE.aux.bcklight_type ^= 1;
    clr_indicator((ind_clr_t)EE.aux.bcklight_type);
    aux_refresh_backlight();
}

/** @brief Toggle beacon on/off (slot). */
void slot_beacon(void)
{
    EE.aux.beacon_on ^= 1;
    clr_indicator((ind_clr_t)EE.aux.beacon_on);
}

/** @brief Toggle memory recall (slot). */
void slot_memory(void)
{
    EE.uif.use_memory ^= 1;
    clr_indicator((ind_clr_t)EE.uif.use_memory);
}

/** @brief Empty slot — no operation. */
void slot_empty(void) {}

/* USE_STROBE / USE_BLOCK: функции объявлены всегда (main_op[] ссылается на них
 * безусловно), гейтинг — только на уровне тела, как slot_batt_ind()/calibration().
 * Иначе выключение флага в config.h ломает линковку. */

/** @brief Enter strobe mode (slot). */
void slot_strobe(void)
{
#ifdef USE_STROBE
    uint8_t start = on_driver();
    driver_mode = MODE_STROB;
    ps.use_prev = true;
    ps.pr_step = step_default;
    ps.start = start;

    ss_cancel();
    start_aux_strobe(strobe_index);
 #endif
}

/** @brief Cycle to next strobe pattern. */
void up_strobe(void)
{
#ifdef USE_STROBE
    stop_aux_if(IDM_STROBE, false);
    if (++strobe_index == STROBE_COUNT) strobe_index = 0;
    start_aux_strobe(strobe_index);
#endif
}

/** @brief Exit strobe back to previous brightness (non-magic-slots). */
void back_strobe(void)
{
#if defined(USE_STROBE) && !defined(USE_MAGIC_SLOTS)
    stop_aux_if(IDM_STROBE, false);
    if (ps.start) off_driver(false, SS_NO);
    else driver_mode = MODE_ON;
    if (ps.use_prev) step_user = ps.pr_step;
    ps.use_prev = false;
    if (!ps.start) set_current(ps.pr_step, SS_NO);
#endif
}

/** @brief Turn off strobe and driver. */
void off_strobe(void)
{
#ifdef USE_STROBE
    stop_aux_if(IDM_STROBE, false);
    off_driver(false, SS_NO);
    if (ps.use_prev) step_default = ps.pr_step;
    ps.use_prev = false;
#endif
}

/** @brief Enter block (locator beacon) mode. */
void slot_block(void)
{
#ifdef USE_BLOCK
    if (driver_mode == MODE_ON)
        driver_mode = MODE_BLOCK_ON;
    else
        driver_mode = MODE_BLOCK;
    aux_off();
#endif
}

/** @brief Enter block + beacon combo mode. */
void slot_block_beacon(void)
{
#ifdef USE_BLOCK
    driver_mode = MODE_BLOCK0;
    aux_off();
#endif
}

/** @brief Exit block mode, return to normal. */
void unblock_drv(void)
{
#ifdef USE_BLOCK
    lock_light_ticks = 0;
    if (driver_mode == MODE_BLOCK_ON || driver_mode == MODE_BLOCK_LIGHT)
        driver_mode = MODE_ON;
    else
        driver_mode = MODE_OFF;
    aux_on();
#endif
}

/** @brief Turn on temporary CMODE_MIN light in lockout mode. */
void lock_light_on(DRIVER_Mode_t prev_mode)
{
#ifdef USE_BLOCK
    saved_block_mode = prev_mode;
    driver_mode = MODE_BLOCK_LIGHT;
    ps.use_prev = false;
    if (aux_needs_dac())
        off_aux_cmd(false);
    ON_POWER();
    down_count = 5;
    vwarning = VWARNING_NONE;
    vwarning_old = VWARNING_NONE;
    direction = DIR_UP;
    mode_current = CMODE_MIN;
    set_curri(CMODE_MIN);
    lock_light_ticks = (uint16_t)(((uint16_t)(EE.usr.lock_min_time & 0x03u) + 1u) * 2400u);
#endif
}

/** @brief Turn off temporary lockout light and restore locked mode. */
void lock_light_off(void)
{
#ifdef USE_BLOCK
    lock_light_ticks = 0;
    off_driver(false, SS_FAST);
    driver_mode = saved_block_mode;
#endif
}


/*******************************************************************************
 *  Reset / calibration
 ******************************************************************************/

/** @brief Reset all settings to EEPROM defaults (engineering menu). */
void reset2def(void)
{
    ee_defaults();
    ob_ResetToDefaults();   /* also reset OB calibration (tc/vc) and slots */
    ui_save = EE.uif.ui_index;
    ui_table_idx = ENG_MENU_IND;
    clr_indicator(IND_GRN);
}

/** @brief Measure Vbat (в кодах Vrefint) and temperature, save vc/tc to Option Bytes.
 * vc/tc допускаются только в окне ±0.29 В (VC_CLIP_MAX/MIN) и ±5 °C
 * (TC_CLIP_MAX/MIN, ~±44 кода); вне окна калибровка отвергается целиком
 * и прежние значения OB сохраняются. */
void calibration(void)
{
#ifdef USE_MENU
    EE_PRINTF("[CAL] start voltage_ok=%u vref=%u temp=%u\r\n",
              (unsigned)voltage_ok,
              (unsigned)vrefint_adc,
              (unsigned)raw_temperature_adc);

    if (!voltage_ok)
    {
        /* Do not save offsets derived from a stale or failed ADC pair. */
        EE_PRINTF("[CAL] rejected: invalid voltage sample\r\n");
        ui_invalid_selection();
        return;
    }

    /* Калибровка в кодах Вrefint (см. adc.h): vc = измеренный код - опорный код.
     * Положительный vc повышает эффективное напряжение (сдвиг e = adc - vc). */
    int16_t vc_delta = (int16_t)vrefint_adc - (int16_t)REF_VREF_CODE;
    int16_t tc_delta = (int16_t)raw_temperature_adc - (int16_t)REF_TEMP_ADC;

    uint8_t ok = 1;
    if (vc_delta > VC_CLIP_MAX || vc_delta < VC_CLIP_MIN)
        ok = 0;
    if (tc_delta > TC_CLIP_MAX || tc_delta < TC_CLIP_MIN)
        ok = 0;
    EE_PRINTF("[CAL] vc_delta=%d (range %d..%d) tc_delta=%d (range %d..%d) ok=%u\r\n",
              (int)vc_delta,
              (int)VC_CLIP_MIN,
              (int)VC_CLIP_MAX,
              (int)tc_delta,
              (int)TC_CLIP_MIN,
              (int)TC_CLIP_MAX,
              (unsigned)ok);

    /* Отказ не трогает OB: предыдущая калибровка остаётся в силе. */
    if (ok)
    {
        ob_SetVc((int8_t)vc_delta);
        ob_SetTc((int8_t)tc_delta);
        EE_PRINTF("[CAL] saved vc=%d tc=%d\r\n",
                  (int)ob_GetVc(),
                  (int)ob_GetTc());
    }
    else
    {
        EE_PRINTF("[CAL] rejected: offsets outside allowed range\r\n");
    }
    if (ok)
        clr_indicator(IND_GRN);
    else
        ui_invalid_selection();
#endif
}

/*******************************************************************************
 *  save_def_ui1 / save_def_ui2
 *
 *  Сохранение текущего шага/режима как значения по умолчанию при включении.
 * Диспетчеризуются через main_op[SAVE_DEF_UI1]/main_op[SAVE_DEF_UI2]
 *  из таблиц: SAVE_DEF_UI1 — ui1_table (5 кликов + HOLD в MODE_ON),
 *  SAVE_DEF_UI2 — ui2_table (тот же жест), оба — eng_table (2/3 клика в OFF).
 *  Не являются частью инженерного меню, поэтому безусловны (как в Proteus84),
 *  а не спрятаны под USE_MENU.
 ******************************************************************************/

/**
 * @brief Общая «мигающая» часть сохранения default: мигнуть бэклайтом/индикатором.
 *
 * Выделена из save_def_ui1/2 — обе функции дублировали тело. Функционально
 * идентично прежнему коду (out_set_stc(0)+пауза для гашения бэклайта).
 */
static void save_def_blink(void)
{
    off_auto_repeat();
    if (EE.uif.off_bckl)
    {
        out_set_stc(0); wait_ms(30);
        out_set_stc(get_step_tc(step_user));
    }
    else
        clr_indicator(IND_GRN);
}

/** @brief Save current UI1 step as power-on default.
 *
 * Сохраняется запрошенный пользователем шаг (step_user), а не
 * термоограниченный step_tc — иначе при активном лимитере дефолтом
 * стала бы заниженная яркость (симметрично save_def_ui2). */
void save_def_ui1(void)
{
    save_def_blink();
    EE.default_ui1 = step_user;
}

/** @brief Save current UI2 mode as power-on default. */
void save_def_ui2(void)
{
    save_def_blink();
    EE.default_ui2 = mode_current;
}

/*******************************************************************************
 *  Engineering menu
 *
 *  Функции объявлены всегда (main_op[]/usr_table ссылаются на них безусловно),
 *  #ifdef USE_MENU гейтит только тело — как slot_batt_ind()/calibration().
 ******************************************************************************/

#ifdef USE_MENU
/** @brief Helper to enter a menu sub-mode and flash current value. */
static void enter_submode(DRIVER_Mode_t mode, uint8_t count)
{
    driver_mode = mode;
    aux_count(count);
}

/** @brief Helper to finish a menu sub-mode: flash new count and return to OFF. */
static void menu_submode_done(void)
{
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
}
#endif

/** @brief Enter moonlight level setting mode (engineering menu). */
void enter_ml_mode(void)
{
#ifdef USE_MENU
    enter_submode(MODE_MOON_LEVEL, ob_GetMl());
#endif
}

/** @brief Set moon level from click count (engineering menu). */
void ml_clickX(void)
{
#ifdef USE_MENU
    ob_SetMl(clicks_count);
    menu_submode_done();
#endif
}

/* Верхняя граница ожидания отпускания кнопки после входа/выхода из меню.
 * Пока кнопка держится, FSM в главном цикле ещё не тронул событие — иначе
 * удержание превратилось бы в HOLD уже в новом UI. */
#define MENU_ENTRY_RELEASE_MS   3000u

#ifdef USE_MENU
/** @brief Мигнуть индикатором cnt раз (для подтверждения входа в меню).
 * @param cnt     Число вспышек.
 * @param on_ms   Длительность свечения/паузы, мс.
 * @param device  Битовая маска LED (RED_LED и/или GRN_LED).
 */
static __attribute__((noinline)) void menu_entry_flash(uint8_t cnt, uint16_t on_ms, LED_Bits_t device);
#endif /* USE_MENU */

#ifdef USE_MENU
/** @brief Ждать отпускания кнопки (максимум MENU_ENTRY_RELEASE_MS).
 *  Вызывается после входа/выхода из меню, пока FSM в главном цикле ещё не
 *  тронул событие — иначе удержание попало бы в новый UI как HOLD. */
static void menu_wait_release(void)
{
    uint16_t count = (uint16_t)(MENU_ENTRY_RELEASE_MS / 10u);
    while (Pressed && count--)
        wait_ms(10);
}
#endif /* USE_MENU */

/** @brief Clamp a UI index from EE to a valid dispatch table (1..3).
 *
 * A corrupted/uninitialised EE byte must never select the tactical bypass
 * (0) or a value ui_table_lookup rejects (dead UI, reboot only).
 */
static uint8_t ui_index_clamp(uint8_t idx)
{
    if (idx < 1u || idx > 3u)
        return 1u;
    return idx;
}

/** @brief Exit engineering menu and restore saved UI index/beacon state.
 *
 * Обязательно возвращает driver_mode в MODE_OFF: выход удержанием из
 * подменю (MODE_MOON_LEVEL/BM/BT/BL/UIS/CTK_H) иначе оставил бы режим,
 * для которого в таблицах UI1/UI2 нет записей (мёртвый UI до сброса). */
void menu_exit(void)
{
#ifdef USE_MENU
    ui_select_from_slot = false;
    ui_save = ui_index_clamp(ui_save);
    EE.uif.ui_index = ui_save;
    ui_table_idx = ui_save;
    menu_beacon_mute = false;
    driver_mode = MODE_OFF;
    hold_guard = Pressed;

    /* Подтверждение выхода: 1 зелёная вспышка 500 мс, затем ожидание
     * отпускания кнопки, чтобы HOLD-повторы не попали в новый UI. */
    menu_entry_flash(1, 500, GRN_LED | RED_LED);
    menu_wait_release();

    /* Применить цвет и состояние подсветки сразу при выходе из меню */
    aux_refresh_backlight();
#endif
}

/* Задержка дебаунс-окна входа в меню при подаче питания (как delay_4ms(255)
 * в оригинале Proteus IID: ~1 с удержания кнопки). */
#define MENU_ENTRY_DEBOUNCE_MS  1000u

#ifdef USE_MENU
/** @brief Flash one or more menu indicator LEDs.
 * @param cnt Number of flashes.
 * @param on_ms LED on/off duration in milliseconds.
 * @param device LED bitmask (RED_LED and/or GRN_LED).
 */
static __attribute__((noinline)) void menu_entry_flash(uint8_t cnt, uint16_t on_ms, LED_Bits_t device)
{
    for (uint8_t i = 0; i < cnt; i++)
    {
        if (device & RED_LED) on_red_h();
        if (device & GRN_LED) on_grn_h();

        wait_ms(on_ms);

        if (device & RED_LED) off_red();
        if (device & GRN_LED) off_grn();

        wait_ms(on_ms);
    }
}
#endif /* USE_MENU */

/**
 * @brief Войти в инженерное/пользовательское меню при подаче питания.
 *
 * Вызывается из main() при старте. Сначала на 1 с индицирует меню, доступное
 * при текущем напряжении питания (красный — инженерное, зелёный — пользовательское),
 * затем, если кнопка нажата, подтверждает нажатие удержанием в течение
 * дебаунс-окна и входит в выбранное по напряжению меню (как в оригинале
 * Proteus IID enter_menu()):
 *  - Vbat > ENG_VOLTAGE_MV (4.5 В, стенд/USB) -> инженерное меню, 6 красных;
 *  - иначе (аккумулятор)                          -> меню пользователя, 3 зелёных.
 * Меню хранится только в runtime ui_table_idx (session-only); выход/перезагрузка
 * возвращают обычный UI. EE.uif.ui_index перед входом сохраняется в ui_save.
 */
void enter_menu_at_boot(void)
{
#ifdef USE_MENU
    /* Выбор меню по напряжению (ADC уже готов, см. main). */
    bool is_eng = voltage_ok && (vrefint_adc < MV_GE(ENG_VOLTAGE_MV));

    /* Индикация доступного меню до опроса кнопки: 0.5 с. */
    menu_entry_flash(1, 500, is_eng ? RED_LED : GRN_LED);

    if (!Pressed) return;

    wait_ms(MENU_ENTRY_DEBOUNCE_MS);

    if (!Pressed) return;

    ui_save = EE.uif.ui_index;
    menu_beacon_mute = true;
    stop_aux_if(IDM_BEACON, true);

    ui_table_idx = is_eng ? ENG_MENU_IND : USR_MENU_IND;
    menu_entry_flash(is_eng ? 6 : 3, is_eng ? 200 : 100,
                     is_eng ? RED_LED : GRN_LED);

    /* Ждём отпускания кнопки, чтобы FSM (button_tick) не принял удержание за
     * свежее нажатие и не выполнил HOLD -> MENU_EXIT при первом же тике.
     * Если кнопка всё ещё зажата (таймаут ожидания), hold_guard подавит
     * первый HOLD-реплей catch-up последовательности. */
    menu_wait_release();
    hold_guard = Pressed;
#endif
}

/** @brief Enter temperature calibration (chtk) mode. */
void enter_chtk_mode(void)
{
#ifdef USE_MENU
    enter_submode(MODE_CTK_H, EE.usr.t_calibr);
#endif
}

/**
 * @brief Check a click count against the range of one menu setting.
 * @param min_clicks Smallest valid number of clicks.
 * @param max_clicks Largest valid number of clicks.
 * @return true when the current click count is valid.
 *
 * Invalid input is reported before any assignment is made, so the previous
 * setting remains intact instead of being truncated by a bit-field.
 */
#ifdef USE_MENU
static bool menu_clicks_valid(uint8_t min_clicks, uint8_t max_clicks)
{
    if (clicks_count >= min_clicks && clicks_count <= max_clicks)
        return true;

    ui_invalid_selection();
    return false;
}
#endif /* USE_MENU */

/**
 * @brief Show a red indication for an invalid user-menu selection.
 *
 * Kept public because the dispatch table owns the magic-slot range check,
 * while LED control belongs to this UI module.
 */
void ui_invalid_selection(void)
{
    off_auto_repeat();
    aux_err(AWU_15MS);
}

/** @brief Set t_calibr from click count. */
void chtk_clickX(void)
{
#ifdef USE_MENU
    if (menu_clicks_valid(1u, 15u))
    {
        EE.usr.t_calibr = clicks_count;
        menu_submode_done();
    }
    driver_mode = MODE_OFF;
#endif
}

/*******************************************************************************
 *  User settings: beacon / backlight / UI select / magic slots
 ******************************************************************************/

/** @brief Toggle external aux mode. */
void slot_eauxmode(void)
{
#ifdef USE_MENU
    clr_indicator(IND_GRN);
#endif
}

/** @brief Toggle IPS mode. */
void slot_ipsmode(void)
{
#ifdef USE_MENU
    EE.usr.id_plus_sid ^= 1;
    clr_indicator((ind_clr_t)EE.usr.id_plus_sid);
#endif
}

/** @brief Enter beacon mode selection (user menu). */
void enter_bm_mode(void)
{
#ifdef USE_MENU
    enter_submode(MODE_BM, (uint8_t)(EE.aux.beacon_mode + 1u));
#endif
}

/** @brief Set beacon mode from click count (user menu). */
void bm_clickX(void)
{
#ifdef USE_MENU
    if (menu_clicks_valid(1u, 4u))
    {
        EE.aux.beacon_mode = clicks_count - 1u;
        menu_submode_done();
    }
    driver_mode = MODE_OFF;
#endif
}

/** @brief Enter beacon type selection (user menu). */
void enter_bt_mode(void)
{
#ifdef USE_MENU
    enter_submode(MODE_BT, (uint8_t)((EE.aux.beacon_type >> 1) + 1u));
#endif
}

/** @brief Set beacon type from click count (user menu). */
void bt_clickX(void)
{
#ifdef USE_MENU
    if (menu_clicks_valid(1u, 3u))
    {
        EE.aux.beacon_type = (uint8_t)(1u << (clicks_count - 1u));
        menu_submode_done();
    }
    driver_mode = MODE_OFF;
#endif
}

/** @brief Enter backlight color selection (user menu). */
void enter_bl_mode(void)
{
#ifdef USE_MENU
    enter_submode(MODE_BL, (uint8_t)(EE.aux.bcklight_color + 1u));
#endif
}

/** @brief Set backlight color from click count (user menu). */
void bl_clickX(void)
{
#ifdef USE_MENU
    if (menu_clicks_valid(1u, 2u))
    {
        EE.aux.bcklight_color = clicks_count - 1u;
        menu_submode_done();
    }
    driver_mode = MODE_OFF;
#endif
}

/** @brief Enter UI selection mode (user menu). */
void enter_uis_mode(void)
{
#ifdef USE_MENU
    enter_submode(MODE_UIS, ui_save);
#endif
}

/** @brief Set UI index from click count (user menu), valid only for 1..3. */
void uis_clickX(void)
{
#ifdef USE_MENU
    /* The dispatch tables support only UI1=1 and UI2 variants=2/3.  Do not
     * clamp a bad selection: that would silently choose a different UI. */
    if (!menu_clicks_valid(1u, 3u))
    {
        driver_mode = MODE_OFF;
        if (ui_select_from_slot)
        {
            ui_table_idx = ui_save;
            ui_select_from_slot = false;
        }
        return;
    }
    ui_save = clicks_count;

    if (ui_select_from_slot)
    {
        /* A slot selection is not inside the user menu, so apply the choice
         * immediately instead of waiting for menu_exit().  The normal
         * 5-second persistence path will save the EE shadow diff. */
        EE.uif.ui_index = clicks_count;
        ui_table_idx = clicks_count;
        ui_select_from_slot = false;
    }
    menu_submode_done();
#endif
}

/** @brief Enter lockout min-light enable/disable configuration (user menu). */
void enter_lm_en_mode(void)
{
#ifdef USE_MENU
    enter_submode(MODE_LM_EN, EE.usr.lock_min_en ? 2u : 1u);
#endif
}

/** @brief Set lockout min-light enable/disable from click count (user menu). */
void lm_en_clickX(void)
{
#ifdef USE_MENU
    if (menu_clicks_valid(1u, 2u))
    {
        EE.usr.lock_min_en = (clicks_count == 2u);
        menu_submode_done();
    }
    driver_mode = MODE_OFF;
#endif
}

/** @brief Enter lockout min-light duration configuration (user menu). */
void enter_lm_time_mode(void)
{
#ifdef USE_MENU
    enter_submode(MODE_LM_TIME, (uint8_t)(EE.usr.lock_min_time + 1u));
#endif
}

/** @brief Set lockout min-light duration from click count (user menu). */
void lm_time_clickX(void)
{
#ifdef USE_MENU
    if (menu_clicks_valid(1u, 4u))
    {
        EE.usr.lock_min_time = clicks_count - 1u;
        menu_submode_done();
    }
    driver_mode = MODE_OFF;
#endif
}

/*******************************************************************************
 *  Magic slots
 ******************************************************************************/

/** @brief Enter magic slot selection mode. */
void enter_magic_mode(void)
{
#ifdef USE_MAGIC_SLOTS
    driver_mode = MODE_MAGIC;
    clr_indicator(IND_GRN);
#else
    (void)0;
#endif
}

/** @brief Exit magic slot mode. */
void exit_magic_mode(void)
{
#ifdef USE_MAGIC_SLOTS
    driver_mode = MODE_OFF;
    clr_indicator(IND_GRN);
#else
    (void)0;
#endif
}

/** @brief Select a magic slot by current click count. */
void select_slot(void)
{
#ifdef USE_MAGIC_SLOTS
    uint8_t slot = clicks_count - 1u;
    enter_submode((DRIVER_Mode_t)(MODE_SLOT01 + slot), ob_GetSlot(slot));
#else
    (void)0;
#endif
}

/** @brief Store click count into currently selected slot. */
void slotN_clickX(void)
{
#ifdef USE_MAGIC_SLOTS
    uint8_t slot = driver_mode - MODE_SLOT01;
    if (!menu_clicks_valid((uint8_t)SLOT_OP_FIRST,
                           (uint8_t)SLOT_OP_LAST))
    {
        /* Keep the old assignment and return to the slot picker. */
        driver_mode = MODE_MAGIC;
        return;
    }

    aux_count(clicks_count);
    ob_SetSlot(slot, clicks_count);
    driver_mode = MODE_MAGIC;
#else
    (void)0;
#endif
}

/**
 * @brief Enter quick UI selection from a magic slot.
 *
 * The slot action reuses MODE_UIS/uis_clickX, but switches the dispatcher to
 * the user-settings table for the duration of the 1/2/3-click selection.
 */
void slot_ui_select(void)
{
#ifdef USE_MAGIC_SLOTS
    ui_save = ui_table_idx;
    ui_select_from_slot = true;
    /* A slot may be assigned to a HOLD gesture.  Suppress the first HOLD
     * repeat in MODE_UIS until that original physical hold is released. */
    hold_guard = Pressed;
    ui_table_idx = USR_MENU_IND;
    driver_mode = MODE_UIS;
    aux_count(ui_save);
#else
    (void)0;
#endif
}

/*******************************************************************************
 *  UI2 helpers and actions
 ******************************************************************************/

/** @brief Convert UI2 mode index to brightness step. */
uint16_t get_stepi(uint8_t index)
{
    return modes[index];
}

/** @brief Clamp mode to voltage-limited ceiling. */
static __attribute__((noinline)) uint8_t get_up_mode(uint8_t mode)
{
    uint8_t up_mode = MIN(volt_stop_mode, volt_max_mode);
    return MIN(up_mode, mode);
}

/** @brief Set brightness for a UI2 mode with fast soft-start. */
void set_curri(uint8_t cmode)
{
    set_current(get_stepi(cmode), SS_FAST);
}

/** @brief Enter given mode (internal helper). */
static __attribute__((noinline)) void on_mode(uint8_t m)
{
    ps.use_prev = false;
    on_driver();
    set_curri(m);
}

/** @brief Go to moon mode (UI2). */
void on_moon_ui2(void)
{
    mode_current = CMODE_MOON;
    on_mode(mode_current);
}

/** @brief Go to minimum mode (UI2, voltage-clamped). */
void on_min_ui2(void)
{
    mode_current = get_up_mode(CMODE_MIN);
    on_mode(mode_current);
}

/** @brief Go to memorised/default mode (UI2). */
void on_xmode_ui2(void)
{
    mode_current = get_up_mode(EE.uif.use_memory ? mode_default : EE.default_ui2);
    on_mode(mode_current);
}

/** @brief Cycle up to next mode (UI2). */
void mode_up_ui2(void)
{
    ps.use_prev = false;
    uint8_t up_mode = get_up_mode(CMODE_MAX);
    if (mode_current < up_mode)
        mode_current++;
    else
    {
        if (mode_current != CMODE_MOON)
            mode_current = CMODE_MIN;
    }
    set_curri(mode_current);
}

/** @brief Find next mode down, dynamically accounting for thermal limiter. */
static uint8_t get_mode_down_ui2(void)
{
    if (mode_current > CMODE_MID && limiter > 0)
    {
        uint16_t cur = get_step_tc(step_user);
        for (uint8_t m = mode_current - 1; m > CMODE_MIN; m--)
        {
            if (modes[m] < cur)
                return m;
        }
        return CMODE_MIN;
    }

    return mode_current - 1;
}

/** @brief Cycle down to previous mode (UI2). */
void mode_down_ui2(void)
{
    if (mode_current > CMODE_MIN)
    {
        ps.use_prev = false;
        mode_current = get_mode_down_ui2();
        set_curri(mode_current);
    }
}

/** @brief Step down (wrap-around) in UI2. */
void step_down_ui2(void)
{
    if (mode_current != CMODE_MOON)
    {
        ps.use_prev = false;
        mode_current = (mode_current > CMODE_MIN) ? get_mode_down_ui2() : get_up_mode(CMODE_MAX);
        set_curri(mode_current);
    }
}

/** @brief Toggle turbo (max mode) with prev-mode recall (UI2). */
void on_turbo_ui2(void)
{
    if (mode_current == CMODE_MOON) return;

    if (!ps.use_prev)
    {
        ps.use_prev = true;
        ps.start = on_driver();
        ps.pr_step = mode_current;
        mode_current = get_up_mode(CMODE_TURBO);
        set_curri(mode_current);
    }
    else
    {
        ps.use_prev = false;
        if (ps.start)
        {
            mode_current = CMODE_OFF;
            off_driver(true, SS_FAST);
        }
        else
        {
            mode_current = ps.pr_step;
            set_curri(mode_current);
        }
    }
}

/*******************************************************************************
 *  Tactical mode
 ******************************************************************************/

/** @brief Handle tactical momentary mode (press = on, release = off).
 *
 * Грань-триггерный режим: DAC/I2C пишутся только при смене состояния
 * кнопки, а не каждый 6 мс тик (ранее set_current/OFF_POWER вызывались
 * непрерывно, в т.ч. по выключенному питанию драйвера). */
static void handle_tactical(void)
{
    if (Pressed)
    {
        if (driver_mode != MODE_ON)
        {
            on_driver();
            set_current(step_default, SS_NO);
        }
    }
    else if (driver_mode != MODE_OFF)
    {
        off_driver(false, SS_NO);
    }
}

/** @brief Initialise UI state variables. */
void ui_init(void)
{
    memset(&ps, 0, sizeof(ps));
    stop_bit = false;
    need_save = 0;
    direction = DIR_UP;
    ui1_was_pressed = false;
    ui_select_from_slot = false;
    hold_guard = false;
    mode_current = CMODE_OFF;
    ui_table_idx = ui_index_clamp(EE.uif.ui_index);
    step_default = EE.default_ui1;   // восстановить сохранённую яркость
    mode_default = EE.default_ui2;   // восстановить сохранённый режим
    aux_refresh_backlight();
}

/**
 * @brief Main UI process — tick at 160 Hz.
 *
 * Handles tactical mode bypass, long-click/max-clicks per UI,
 * hold timing, direction timeout, and dispatches button events via
 * ui_table_lookup.
 */
void ui_process(void)
{
    if (hold_guard && !Pressed)
        hold_guard = false;

    /* Temporary CMODE_MIN light countdown in lockout mode */
    if (driver_mode == MODE_BLOCK_LIGHT)
    {
        if (lock_light_ticks > 0)
        {
            lock_light_ticks--;
            if (lock_light_ticks == 0)
            {
                lock_light_off();
            }
        }
    }

    uint8_t ui_idx = ui_table_idx;

    /* Tactical mode bypasses FSM events entirely */
    if (ui_idx == 0)
    {
        handle_tactical();
        return;
    }

    /* Long-click and HOLD timing per UI.  Применяются один раз при смене
     * ui_table_idx, а не каждый тик.  UI2 repeats a mode change only
     * after the previous fast soft-start has had time to finish. */
    if (ui_idx != ui_cfg_idx)
    {
        ui_cfg_idx = ui_idx;

        if (ui_idx == 1)
        {
            button_set_long_click_ticks(BTN_LONG_CLICK_TICKS);
            button_set_hold_ticks(BTN_HOLD_TICKS);
        }
        else if (ui_idx == 2)
        {
            button_set_long_click_ticks(LONG_PRESS_TIME_UI2);
            button_set_hold_ticks(VERY_LONG_PRESS_UI2);
            button_set_repeat_ticks(VERY_LONG_PRESS_UI2);
        }
        else if (ui_idx == 3)
        {
            button_set_long_click_ticks(VERY_LONG_PRESS_UI2);
            button_set_hold_ticks(VERY_LONG_PRESS_UI2);
            button_set_repeat_ticks(VERY_LONG_PRESS_UI2);
        }
        else
            button_set_long_click_ticks(VERY_LONG_PRESS_UI2);

        /* Max clicks per UI */
        if (ui_idx == 5 || ui_idx == 6)
            button_set_max_clicks(BTN_MAX_CLICKS_MENU);
        else
            button_set_max_clicks(BTN_MAX_CLICKS);
    }

    /* The AVR scanner flipped direction on every physical release, including
     * a release after HOLD.  BTN_EVT_RELEASED deliberately excludes held
     * presses, so this must be an edge check outside event dispatch. */
    bool pressed = Pressed;
    if (ui_idx == 1 && ui1_was_pressed && !pressed)
    {
        direction ^= 0x01;
        bt_r_time = 0;
    }
    /* Direction timeout for UI1 */
    if (!pressed)
    {
        if (bt_r_time < 0xFF) bt_r_time++;
        if (bt_r_time >= DIRECTION_TIMEOUT_UI1) direction = DIR_UP;
    }
    else
    {
        bt_r_time = 0;
    }
    ui1_was_pressed = (ui_idx == 1) && pressed;

    /* Consume pending button event */
    const btn_evt_t *evt = button_get_event();
    if (!evt) return;

    clicks_count = evt->clicks;

    switch (evt->type)
    {
        case BTN_EVT_PRESSED:
            tick_5s_reset();
            /* В режимах блокировки работает только жест разблокировки;
             * каждое нажатие подтверждается красной вспышкой DANG_RED. */
            if (driver_mode == MODE_BLOCK || driver_mode == MODE_BLOCK0 ||
                driver_mode == MODE_BLOCK_ON || driver_mode == MODE_BLOCK_LIGHT)
            {
                aux_dang();
            }
            if (ui_idx == 1)
            {
                /* The UI1 ceiling is a sag ratchet.  A press starts a new
                 * activation attempt, so clear a stale historical cap only
                 * after a confirmed healthy voltage measurement
                 * (vbat > 3100 мВ ⟺ vrefint_adc <= MV_GE(3101)). */
                if (voltage_ok &&
                    ((int32_t)vrefint_adc - (int32_t)ob_GetVc() <= (int32_t)MV_GE(3101u)))
                {
                    reset_max_step();
                }

                /* Never turn an uninitialised/failed zero sample into a
                 * permanent low-voltage ratchet.  Any valid reading,
                 * including a genuinely depleted cell, still follows the
                 * normal low-voltage ceiling and shutdown paths. */
                if (voltage_ok)
                {
                    /* limiter is applied once when the physical target is
                     * written; keep the battery ceiling in user-step space. */
                    max_step = ratchet_max_step();
                }
                stop_aux_if(IDM_BEACON, false);
                if (step_tc >= max_step || limiter) direction = DIR_DOWN;
            }
            break;

        case BTN_EVT_RELEASED:
            tick_5s_reset();
            stop_bit = false;
            ui_table_lookup(driver_mode, evt, false);
            break;

        case BTN_EVT_HOLD:
        {
            tick_5s_reset();
            uint8_t prev_mode = driver_mode;

            /* Удержание, которым был вызван slot/меню (или вход в меню при
             * старте), не должно попадать в новый UI как HOLD-repeat. */
            if (hold_guard)
                break;

            ui_table_lookup(driver_mode, evt, false);

            /* Hold in any slot programming mode -> exit back to magic menu */
            if (driver_mode >= MODE_SLOT01 && driver_mode <= MODE_SLOT10
                && prev_mode == driver_mode)
            {
                exit_magic_mode();
            }

            /* UI1 hold speed: first hold from OFF -> pause; ON -> fastest
             * repeat (FASTEST_RAMP_REPEAT_UI1; never 0, which would disable
             * repetition in button.c). */
            if (ui_idx == 1 && prev_mode == MODE_OFF && evt->clicks == 0)
                button_set_repeat_ticks(LONG_HOLD_RAMP_PAUSE_UI1);
            else if (ui_idx == 1 && prev_mode == MODE_ON)
                button_set_repeat_ticks(FASTEST_RAMP_REPEAT_UI1);
            break;
        }

        case BTN_EVT_CLICK_SEQ:
            tick_5s_reset();
            /* UI2 handles long clicks on release.  Do not dispatch the same
             * long gesture again when its click-sequence timeout expires. */
            if ((ui_idx == 2U || ui_idx == 3U) && evt->long_mask)
                break;
            ui_table_lookup(driver_mode, evt, true);
            break;

        default:
            break;
    }
}
