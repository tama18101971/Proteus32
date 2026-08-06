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
#include "ob.h"
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

uint8_t VLPT = LONG_HOLD_UI1;
DRIVER_Mode_t driver_mode = MODE_OFF;
bool stop_bit = false;

uint16_t step_tc;
uint16_t step_tactic;
uint16_t step_user;
uint16_t max_step = MAX_STEP;
uint16_t step_default = DEFAULT_STEP;

RUMP_Dir_t direction = DIR_UP;

PSTEP_t ps;

uint8_t need_save = false;

uint8_t clicks_count = 0;
uint8_t ui_table_idx = 1;        // runtime selector for dispatch tables
uint8_t ui_save = 0;
uint8_t beacon_save = 0;
bool menu_beacon_mute = false;   // session-only AUX beacon mute while in menu
uint8_t bcklight_type_save = 0;
uint8_t strobe_index = 0;

static uint8_t bt_r_time = 0;    // direction timeout counter
static bool ui1_was_pressed;

/*******************************************************************************
 *  UI2 discrete brightness table -- scaled to Proteus003 9-bit range (0..511)
 ******************************************************************************/

static const uint16_t modes[MODES_QTY] = {0, 20, 74, 136, 220, 360, 511};

/*******************************************************************************
 *  Power / DAC helpers (Proteus003)
 ******************************************************************************/

/** @brief Power-on sequence: reset limiters, enable DAC, init I2C. */
void ON_POWER(void)
{
    EE_PRINTF("[UI] ON PWR.\r\n");
    reset_limiters();
    dac_pwrpin_on();
    i2c_init(400000);
    for (volatile uint32_t i = 0; i < 24000; i++);  // ~5 ms DAC wake-up
}

/** @brief Power-off sequence: deinit I2C, disable DAC. */
void OFF_POWER(void)
{
    EE_PRINTF("[UI] OFF PWR.\r\n");
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

    EE.usr.eaux_onoff = false;
    EE.usr.id_plus_sid = false;
    EE.usr.t_calibr = 6;

    EE.default_ui1 = DEFAULT_STEP;
    EE.default_ui2 = DEFAULT_MODE;

    EE.uif.off_bckl       = true;         
}

/**
 * @brief Clamp brightness target to thermal ceiling.
 * @param step Requested brightness step.
 * @return max(step - limiter, 0), preserving the requested step separately.
 */
uint16_t get_step_tc(uint16_t step)
{
    if (thermal_fault_active())
        return 0u;

    return (step > limiter) ? (uint16_t)(step - limiter) : 0u;
}

/**
 * @brief Get minimum brightness floor.
 * @param step If non-zero, returns moon level offset; otherwise 0.
 * @return (ml << 1) + BASE_MOON_LEVEL or 0.
 */
uint16_t get_min_level(uint16_t step)
{
    uint16_t ret = step ? ((ob_GetMl() << 1) + BASE_MOON_LEVEL) : 0;
    EE_PRINTF("[UI] min_level: ml=%u step=%u -> ret=%u\r\n",
              (unsigned)ob_GetMl(), (unsigned)step, (unsigned)ret);
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
    uint32_t result = (x * x * x * 16) >> 19;

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
    if (EE.uif.ui_index)
        step_tactic = 0;

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
        step_tactic = MAX_STEP;
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
    EE_PRINTF("[UI] OFF DRV false\r\n");
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
    EE.aux.bcklight_type = !EE.aux.bcklight_type;
    clr_indicator((ind_clr_t)EE.aux.bcklight_type);
}

/** @brief Toggle beacon on/off (slot). */
void slot_beacon(void)
{
    if (ui_table_idx == 6)
    {
        beacon_save = !beacon_save;
        clr_indicator((ind_clr_t)beacon_save);
    }
    else
    {
        EE.aux.beacon_on = !EE.aux.beacon_on;
        clr_indicator((ind_clr_t)EE.aux.beacon_on);
    }
}

/** @brief Toggle memory recall (slot). */
void slot_memory(void)
{
    EE.uif.use_memory = !EE.uif.use_memory;
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
    driver_mode = MODE_OFF;
    aux_on();
#endif
}


/*******************************************************************************
 *  Reset / calibration
 ******************************************************************************/

/** @brief Reset all settings to EEPROM defaults (engineering menu). */
void reset2def(void)
{
    ee_defaults();
    ui_save = EE.uif.ui_index;
    ui_table_idx = ENG_MENU_IND;
    beacon_save = EE.aux.beacon_on;
    clr_indicator(IND_GRN);
}

/** @brief Measure Vbat (в кодах Vrefint) and temperature, save vc/tc to Option Bytes. */
void calibration(void)
{
#ifdef USE_MENU
    if (!voltage_ok)
    {
        /* Do not save offsets derived from a stale or failed ADC pair. */
        clr_indicator(IND_RED);
        return;
    }

    /* Калибровка в кодах Вrefint (см. adc.h): vc = измеренный код - опорный код.
     * Положительный vc повышает эффективное напряжение (сдвиг e = adc - vc). */
    int16_t vc_delta = (int16_t)vrefint_adc - (int16_t)REF_VREF_CODE;
    int16_t tc_delta = (int16_t)raw_temperature_adc - (int16_t)REF_TEMP_ADC;

    uint8_t ok = 1;
    if (vc_delta > VC_CLIP_MAX || vc_delta < VC_CLIP_MIN)
    {
        ok = 0;
        vc_delta = 0;
    }
    if (tc_delta > TC_CLIP_MAX || tc_delta < TC_CLIP_MIN)
    {
        ok = 0;
        tc_delta = 0;
    }

    ob_SetVc((int8_t)vc_delta);
    ob_SetTc((int8_t)tc_delta);
    clr_indicator((ind_clr_t)ok);
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

/** @brief Save current UI1 step as power-on default. */
void save_def_ui1(void)
{
    save_def_blink();
    EE.default_ui1 = step_tc;
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

/** @brief Enter moonlight level setting mode (engineering menu). */
void enter_ml_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_MOON_LEVEL;
    aux_count(ob_GetMl());
#endif
}

/** @brief Set moon level from click count (engineering menu). */
void ml_clickX(void)
{
#ifdef USE_MENU
    ob_SetMl(clicks_count);
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
#endif
}

/* Верхняя граница ожидания отпускания кнопки после входа/выхода из меню.
 * Пока кнопка держится, FSM в главном цикле ещё не тронул событие — иначе
 * удержание превратилось бы в HOLD уже в новом UI. */
#define MENU_ENTRY_RELEASE_MS   3000u

/** @brief Мигнуть индикатором cnt раз (для подтверждения входа в меню).
 * @param cnt     Число вспышек.
 * @param on_ms   Длительность свечения/паузы, мс.
 * @param is_grn  true — зелёный ИД, false — красный.
 */
static void menu_entry_flash(uint8_t cnt, uint16_t on_ms, bool is_grn);

/** @brief Ждать отпускания кнопки (максимум MENU_ENTRY_RELEASE_MS).
 *  Вызывается после входа/выхода из меню, пока FSM в главном цикле ещё не
 *  тронул событие — иначе удержание попало бы в новый UI как HOLD. */
static void menu_wait_release(void)
{
    uint32_t t = 0;
    while (Pressed && (t < MENU_ENTRY_RELEASE_MS))
    {
        wait_ms(10);
        t += 10;
    }
    (void)t;
}

/** @brief Exit engineering menu and restore saved UI index/beacon state. */
void menu_exit(void)
{
#ifdef USE_MENU
    EE.uif.ui_index = ui_save;
    EE.aux.beacon_on = beacon_save;
    ui_table_idx = ui_save;
    menu_beacon_mute = false;

    /* Подтверждение выхода: 1 зелёная вспышка 500 мс, затем ожидание
     * отпускания кнопки, чтобы HOLD-повторы не попали в новый UI. */
    menu_entry_flash(1, 500, true);
    menu_wait_release();
#endif
}

/* Задержка дебаунс-окна входа в меню при подаче питания (как delay_4ms(255)
 * в оригинале Proteus IID: ~1 с удержания кнопки). */
#define MENU_ENTRY_DEBOUNCE_MS  1000u

/** @brief Мигнуть индикатором cnt раз (для подтверждения входа в меню).
 * @param cnt     Число вспышек.
 * @param on_ms   Длительность свечения/паузы, мс.
 * @param is_grn  true — зелёный ИД, false — красный.
 */
static void menu_entry_flash(uint8_t cnt, uint16_t on_ms, bool is_grn)
{
    for (uint8_t i = 0; i < cnt; i++)
    {
        if (is_grn) on_grn_h();
        else        on_red_h();

        wait_ms(on_ms);

        if (is_grn) off_grn();
        else        off_red();

        wait_ms(on_ms);
    }
}

/**
 * @brief Войти в инженерное/пользовательское меню при подаче питания.
 *
 * Вызывается из main() только если кнопка уже нажата к моменту старта.
 * Подтверждает нажатие удержанием в течение дебаунс-окна, затем выбирает меню
 * по напряжению питания (как в оригинале Proteus IID enter_menu()):
 *  - Vbat > ENG_VOLTAGE_MV (4.5 В, стенд/USB) -> инженерное меню, 6 красных;
 *  - иначе (аккумулятор)                          -> меню пользователя, 3 зелёных.
 * Меню хранится только в runtime ui_table_idx (session-only); выход/перезагрузка
 * возвращают обычный UI. EE.uif.ui_index / EE.aux.beacon_on перед входом
 * сохраняются в ui_save/beacon_save, чтобы menu_exit() их восстановил.
 */
void enter_menu_at_boot(void)
{
#ifdef USE_MENU
    if (!Pressed)
        return;

    wait_ms(MENU_ENTRY_DEBOUNCE_MS);
    if (!Pressed)
        return;

    ui_save = EE.uif.ui_index;
    beacon_save = EE.aux.beacon_on;
    /* Маяк в меню не мешает, но EE не трогаем — сессионный RAM-флаг,
     * сбрасывается при выходе (menu_exit) и при любом питании (reset). */
    menu_beacon_mute = true;

    if (voltage_ok && (vrefint_adc < MV_GE(ENG_VOLTAGE_MV)))
    {
        ui_table_idx = ENG_MENU_IND;            /* > 4.5 В */
        menu_entry_flash(6, 200, false);        /* 6 красных */
    }
    else
    {
        ui_table_idx = USR_MENU_IND;            /* аккумулятор */
        menu_entry_flash(3, 100, true);         /* 3 зелёных */
    }

    /* Ждём отпускания кнопки, чтобы FSM (button_tick) не принял удержание за
     * свежее нажатие и не выполнил HOLD -> MENU_EXIT при первом же тике. */
    menu_wait_release();
#endif
}

/** @brief Enter temperature calibration (chtk) mode. */
void enter_chtk_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_CTK_H;
    aux_count(EE.usr.t_calibr);
#endif
}

/** @brief Set t_calibr from click count. */
void chtk_clickX(void)
{
#ifdef USE_MENU
    EE.usr.t_calibr = clicks_count;
    aux_count(clicks_count);
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
    EE.usr.eaux_onoff = !EE.usr.eaux_onoff;
    clr_indicator((ind_clr_t)EE.usr.eaux_onoff);
#endif
}

/** @brief Toggle IPS mode. */
void slot_ipsmode(void)
{
#ifdef USE_MENU
    EE.usr.id_plus_sid = !EE.usr.id_plus_sid;
    clr_indicator((ind_clr_t)EE.usr.id_plus_sid);
#endif
}

/** @brief Enter beacon mode selection (user menu). */
void enter_bm_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_BM;
    aux_count(EE.aux.beacon_mode + 1);
#endif
}

/** @brief Set beacon mode from click count (user menu). */
void bm_clickX(void)
{
#ifdef USE_MENU
    EE.aux.beacon_mode = clicks_count - 1;
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
#endif
}

/** @brief Enter beacon type selection (user menu). */
void enter_bt_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_BT;
    aux_count((EE.aux.beacon_type >> 1) + 1);
#endif
}

/** @brief Set beacon type from click count (user menu). */
void bt_clickX(void)
{
#ifdef USE_MENU
    EE.aux.beacon_type = (1 << (clicks_count - 1));
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
#endif
}

/** @brief Enter backlight color selection (user menu). */
void enter_bl_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_BL;
    aux_count(EE.aux.bcklight_color + 1);
#endif
}

/** @brief Set backlight color from click count (user menu). */
void bl_clickX(void)
{
#ifdef USE_MENU
    EE.aux.bcklight_color = clicks_count - 1;
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
#endif
}

/** @brief Enter UI selection mode (user menu). */
void enter_uis_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_UIS;
    aux_count(ui_save);
#endif
}

/** @brief Set UI index from click count (user menu). Clamped to valid 1..3. */
void uis_clickX(void)
{
#ifdef USE_MENU
    /* Диспетчеру известны только UI1=1 и UI2=2/3 (ui_table_lookup). Другие
     * значения (4..31) привели бы к «мёртвому» ui_table_idx, а 5/6 — к
     * автоматическому входу в меню при следующем включении. Клэмпим. */
    uint8_t sel = clicks_count;
    if (sel < 1u) sel = 1u;
    if (sel > 3u) sel = 3u;
    ui_save = sel;
    aux_count(sel);
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
    uint8_t slot = clicks_count - 1;
    driver_mode = MODE_SLOT01 + slot;
    aux_count(ob_GetSlot(slot));
#else
    (void)0;
#endif
}

/** @brief Store click count into currently selected slot. */
void slotN_clickX(void)
{
#ifdef USE_MAGIC_SLOTS
    uint8_t slot = driver_mode - MODE_SLOT01;
    uint8_t op = (clicks_count >= SLOT_OP_FIRST && clicks_count <= SLOT_OP_LAST)
               ? clicks_count : SLOT_OP_EMPTY;
    aux_count(clicks_count);
    ob_SetSlot(slot, op);
    driver_mode = MODE_MAGIC;
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

/** @brief Cycle down to previous mode (UI2). */
void mode_down_ui2(void)
{
    if (mode_current > CMODE_MIN)
    {
        ps.use_prev = false;
        set_curri(--mode_current);
    }
}

/** @brief Step down (wrap-around) in UI2. */
void step_down_ui2(void)
{
    if (mode_current != CMODE_MOON)
    {
        ps.use_prev = false;
        if (mode_current > CMODE_MIN)
            mode_current--;
        else
            mode_current = get_up_mode(CMODE_MAX);
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

/** @brief Handle tactical momentary mode (press = on, release = off). */
static void handle_tactical(void)
{
    if (Pressed)
    {
        on_driver();
        set_current(step_tactic ? step_tactic : step_default, SS_NO);
    }
    else
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
    mode_current = CMODE_OFF;
    ui_table_idx = EE.uif.ui_index;
    step_default = EE.default_ui1;   // восстановить сохранённую яркость
    mode_default = EE.default_ui2;   // восстановить сохранённый режим
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
    uint8_t ui_idx = ui_table_idx;

    /* Tactical mode bypasses FSM events entirely */
    if (ui_idx == 0)
    {
        handle_tactical();
        return;
    }

    /* Long-click and HOLD timing per UI.  UI2 repeats a mode change only
     * after the previous fast soft-start has had time to finish. */
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
                if (step_tc >= max_step) direction = DIR_DOWN;
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
