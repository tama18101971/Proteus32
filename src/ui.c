/*
 * ui.c
 *
 * UI action functions + ui_process dispatcher.
 *
 * Ported from Proteus84 (AVR) to Proteus003 (CH32V003).
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
#include "app_data.h"
#include "dac.h"
#include "timers.h"
#include "limiters.h"
#include "main.h"
#include "ob.h"

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
uint8_t ramp_speed = 2;          // replaces EE.eng.rump_speed (session only)
uint8_t pid_time = 3;            // replaces EE.eng.pid_time (session only)
uint8_t volt_max_mode = CMODE_TURBO;
uint8_t volt_stop_mode = CMODE_TURBO;  // "без ограничений" по умолчанию (как в Proteus84: MODES_QTY-1);
                                       // динамическое снижение по напряжению батареи не портировано
uint8_t down_count = 0;
uint8_t ui_save = 0;
uint8_t beacon_save = 0;
uint8_t bcklight_type_save = 0;
uint8_t strobe_index = 0;

static uint8_t bt_r_time = 0;    // direction timeout counter

/*******************************************************************************
 *  UI2 discrete brightness table -- scaled to Proteus003 9-bit range (0..511)
 ******************************************************************************/

static const uint16_t modes[MODES_QTY] = {0, 20, 74, 136, 220, 360, 511};

/*******************************************************************************
 *  Power / DAC helpers (Proteus003)
 ******************************************************************************/

void ON_POWER(void)
{
    EE_PRINTF("[UI] ON PWR.\r\n");
    reset_limiters();
    dac_pwrpin_on();
    i2c_init(400000);
    for (volatile uint32_t i = 0; i < 24000; i++);  // ~5 ms DAC wake-up
}

void OFF_POWER(void)
{
    EE_PRINTF("[UI] OFF PWR.\r\n");
    i2c_deinit();
    dac_pwrpin_off();
}

void ee_defaults(void)
{
    // EE.version = EE_VERSION; инициализация в init_ee()

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

    EE.uif.off_bckl       = true;          // ee_defaults() не трогает это поле (остаётся false)
    EE.aux.bcklight_color = BCK_COLOR_GRN; // default: BCK_COLOR_RED
    EE.aux.beacon_mode    = BCON_4S;       // default: BCON_1S
    EE.aux.beacon_on      = false;         // default: true
    EE.aux.beacon_type    = 1;             // default: GRN_LED(2); тестовый стенд хочет RED_LED(1)

}

uint16_t get_step_tc(uint16_t step)
{
    int16_t tc_step = (MAX_STEP - 0);
    uint16_t result = step <= tc_step ? step : (tc_step > 0 ? tc_step : 1);
    return result;
}

uint16_t get_min_level(uint16_t step)
{
    return step ? ((ob_GetMl() << 1) + BASE_MOON_LEVEL) : 0;
}

uint16_t getBright_CRT_9to12Bit(uint16_t val)
{
    if (val < 2) return val;
    if (val < 10) return 1;
    if (val >= 511) return 4095;

    uint32_t x = val;
    uint32_t result = (x * x * x * 16) >> 19;

    if (result == 0) result = 1;

    return (uint16_t)result;
}

void out_set_stc(uint16_t step)
{
    uint16_t out = getBright_CRT_9to12Bit(step) + get_min_level(step);
    if (out > 4095) out = 4095;
    dac7571_set_voltage(out);
}

uint8_t on_driver(void)
{
    EE_PRINTF("[UI] ON DRV.\r\n");

    if (EE.uif.ui_index)
        step_tactic = 0;

    if (driver_mode < MODE_BLOCK_C)
    {
        EE_PRINTF("[UI] blocked, no on\r\n");
        return 1;
    }

    if (driver_mode < MODE_ON)
    {
        ON_POWER();
        vwarning = VWARNING_NONE;
        vwarning_old = VWARNING_NONE;
        direction = DIR_UP;
        driver_mode = MODE_ON;
        return 1;
    }

    return 0;
}

void off_driver(uint8_t reset_step, SoftStartSpeed_t ss_speed)
{
    EE_PRINTF("[UI] OFF DRV.\r\n");

    driver_mode = MODE_OFF;
    direction = DIR_UP;

    set_current(0, ss_speed);

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

    stop_aux_voltage(false);
    stop_aux_beacon(false);
    EE_PRINTF("[UI] AUX stops skipped (debug mode)\r\n");

    // EEPROM write intentionally commented out per integration requirements
    // if (need_save) write_ee();
    need_save = false;

    off_auto_repeat();

    if (reset_step)
        step_tc = 0;
}

void set_current_ll(uint16_t step_target, SoftStartSpeed_t ss_speed)
{
    if (!step_target && step_tc)
        step_default = step_tc;

    if (ss_speed != SS_NO)
        ss_init(step_target, step_tc, ss_speed);
    else
    {
        out_set_stc(step_target);
        step_tc = step_target;
    }
}

void set_current(uint16_t step_target, SoftStartSpeed_t ss_speed)
{
    step_user = step_target;
    set_current_ll(step_target, ss_speed);
    if ((!step_target) && (!ss_speed)) OFF_POWER();
}

uint16_t get_max_step(void)
{
    return MAX_STEP;
}

void off_auto_repeat(void)
{
    if (Pressed)
        stop_bit = true;
}

void step_up_ui1(void)
{
    if (step_user < max_step)
        set_current(++step_user, SS_NO);
}

void step_down_ui1(void)
{
    if (step_user > 1)
        set_current(--step_user, SS_NO);
}

void on_rumpup_ui1(void)
{
    on_driver();
    ps.use_prev = false;
    if (direction)
        step_up_ui1();
    else
        step_down_ui1();
}

void on_rumpdown_ui1(void)
{
    ps.use_prev = false;
    step_down_ui1();
}

void on_xmode_ui1(void)
{
    ps.use_prev = false;
    on_driver();
    set_current(EE.uif.use_memory ? (step_user ? step_user : step_default) : EE.default_ui1, SS_FAST);
}

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

void off_driver_false(void)
{
    EE_PRINTF("[UI] OFF DRV false\r\n");
    driver_mode = MODE_OFF;
    mode_default = mode_current;
    mode_current = CMODE_OFF;
    off_driver(false, SS_FAST);
}

/*******************************************************************************
 *  AUX indicator helper  (AUX calls commented -> EE_PRINTF)
 ******************************************************************************/

static void onoff_indicator(uint8_t onoff)
{
    off_auto_repeat();
    if (onoff)
    {
        EE_PRINTF("[UI] AUX on (green)\r\n");
        // aux_on();
    }
    else
    {
        EE_PRINTF("[UI] AUX off (red)\r\n");
        // aux_off();
    }
}

/*******************************************************************************
 *  Slot action functions
 ******************************************************************************/

void slot_tactical(void)
{
    ui_table_idx = 0;
    onoff_indicator(true);
}

void slot_backlight(void)
{
    EE.aux.bcklight_type = !EE.aux.bcklight_type;
    onoff_indicator(EE.aux.bcklight_type);
}

void slot_beacon(void)
{
    if (ui_table_idx == 6)
    {
        beacon_save = !beacon_save;
        onoff_indicator(beacon_save);
    }
    else
    {
        EE.aux.beacon_on = !EE.aux.beacon_on;
        onoff_indicator(EE.aux.beacon_on);
    }
}

void slot_memory(void)
{
    EE.uif.use_memory = !EE.uif.use_memory;
    onoff_indicator(EE.uif.use_memory);
}

void slot_empty(void) {}

/* USE_STROBE / USE_BLOCK: функции объявлены всегда (main_op[] ссылается на них
 * безусловно), гейтинг — только на уровне тела, как slot_batt_ind()/calibration().
 * Иначе выключение флага в config.h ломает линковку. */

void slot_strobe(void)
{
#ifdef USE_STROBE
    uint8_t start = on_driver();
    driver_mode = MODE_STROB;
    ps.use_prev = true;
    ps.pr_step = step_default;
    ps.start = start;

    uint32_t s = strobe[strobe_index];
    start_aux_cmd(s, true, IDM_STROBE, AWU_120MS);
    EE_PRINTF("[UI] slot_strobe idx=%u start=%u\r\n", strobe_index, start);
#endif
}

void up_strobe(void)
{
#ifdef USE_STROBE
    stop_aux_strobe(false);
    if (++strobe_index == 4) strobe_index = 0;
    EE_PRINTF("[UI] up_strobe idx=%u\r\n", strobe_index);
#endif
}

void back_strobe(void)
{
#if defined(USE_STROBE) && !defined(USE_MAGIC_SLOTS)
    stop_aux_strobe();
    if (ps.start) off_driver(false, SS_NO);
    else driver_mode = MODE_ON;
    if (ps.use_prev) step_user = ps.pr_step;
    ps.use_prev = false;
    if (!ps.start) set_current(ps.pr_step, SS_NO);
#endif
}

void off_strobe(void)
{
#ifdef USE_STROBE
    stop_aux_strobe(false);
    off_driver(false, SS_NO);
    if (ps.use_prev) step_default = ps.pr_step;
    ps.use_prev = false;
#endif
}

void slot_block(void)
{
#ifdef USE_BLOCK
    driver_mode = MODE_BLOCK;
    EE_PRINTF("[UI] AUX info red (block)\r\n");
    // aux_cmd_info_red();
#endif
}

void slot_block_beacon(void)
{
#ifdef USE_BLOCK
    driver_mode = MODE_BLOCK0;
    EE_PRINTF("[UI] AUX info red (block+beacon)\r\n");
    // aux_cmd_info_red();
#endif
}

void unblock_drv(void)
{
#ifdef USE_BLOCK
    driver_mode = MODE_OFF;
    EE_PRINTF("[UI] AUX info green (unblock)\r\n");
    // aux_cmd_info_grn();
#endif
}


/*******************************************************************************
 *  Reset / calibration
 ******************************************************************************/

void reset2def(void)
{
    init_ee();                  // zeroes RAM, sets version
    ee_defaults();              // fills application defaults
    ui_save = EE.uif.ui_index;
    ui_table_idx = ENG_MENU_IND;
    beacon_save = EE.aux.beacon_on;
    onoff_indicator(true);
}

void calibration(void)
{
#ifdef USE_MENU
    // Proteus84 ADC calibration logic -- not directly portable.
    // Keeping original code commented as requested.
    // int8_t vc = (get_adc(ADCMUX_VCC) - REF_VOLTAGE);
    // int8_t tc = (get_adc(ADCMUX_TEMP) - 0x12C);
    // uint8_t ok = true;
    // if (ABS(vc) > 15) { ok = false; vc = 0; }
    // if (ABS(tc) > 31) { ok = false; tc = 0; }
    // ob_SetVc(vc);
    // ob_SetTc(tc);
    // onoff_indicator(ok);
    EE_PRINTF("[UI] calibration (EEPROM writes commented)\r\n");
#endif
}

/*******************************************************************************
 *  save_def_ui1 / save_def_ui2
 *
 *  Сохранение текущего шага/режима как значения по умолчанию при включении.
 *  Диспетчеризуются из базовой ui1_table (main_op[34]/[35], см. ui_table.c) —
 *  не являются частью инженерного меню, поэтому безусловны (как в Proteus84),
 *  а не спрятаны под USE_MENU.
 ******************************************************************************/

void save_def_ui1(void)
{
    off_auto_repeat();
    if (EE.uif.off_bckl)
    {
        out_set_stc(0);
        Delay_Ms(30);
        out_set_stc(step_user);
    }
    else
        onoff_indicator(true);

    EE.default_ui1 = step_tc;
    // need_save = true;  // EEPROM write commented
    EE_PRINTF("[UI] save_def_ui1 step=%u (save commented)\r\n", (unsigned)step_tc);
}

void save_def_ui2(void)
{
    off_auto_repeat();
    if (EE.uif.off_bckl)
    {
        out_set_stc(0);
        Delay_Ms(30);
        out_set_stc(step_user);
    }
    else
        onoff_indicator(true);

    EE.default_ui2 = mode_current;
    // need_save = true;  // EEPROM write commented
    EE_PRINTF("[UI] save_def_ui2 mode=%u (save commented)\r\n", (unsigned)mode_current);
}

/*******************************************************************************
 *  Engineering menu
 *
 *  Функции объявлены всегда (main_op[]/usr_table ссылаются на них безусловно),
 *  #ifdef USE_MENU гейтит только тело — как slot_batt_ind()/calibration().
 ******************************************************************************/

void enter_rs_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_RAMP_SPEED;
    //EE_PRINTF("[UI] enter rs mode ramp_speed=%u\r\n", (unsigned)(ramp_speed + 1));
    aux_count(ramp_speed + 1);
#endif
}

void rs_clickX(void)
{
#ifdef USE_MENU
    ramp_speed = clicks_count - 1;
    //EE_PRINTF("[UI] rs_clickX ramp_speed=%u\r\n", (unsigned)ramp_speed);
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
#endif
}

void enter_ml_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_MOON_LEVEL;
    //EE_PRINTF("[UI] enter ml mode min_level=%u\r\n", (unsigned)ob_GetMl());
    aux_count(ob_GetMl());
#endif
}

void ml_clickX(void)
{
#ifdef USE_MENU
    ob_SetMl(clicks_count);
    //EE_PRINTF("[UI] ml_clickX min_level=%u\r\n", (unsigned)ob_GetMl());
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
#endif
}

void enter_pt_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_PIP_TIME;
    //EE_PRINTF("[UI] enter pt mode pid_time=%u\r\n", (unsigned)pid_time);
    aux_count(pid_time);
#endif
}

void pt_clickX(void)
{
#ifdef USE_MENU
    pid_time = clicks_count;
    //EE_PRINTF("[UI] pt_clickX pid_time=%u\r\n", (unsigned)pid_time);
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
#endif
}

void menu_exit(void)
{
#ifdef USE_MENU
    EE.uif.ui_index = ui_save;
    EE.aux.beacon_on = beacon_save;
    ui_table_idx = ui_save;
    // write_ee();  // EEPROM write commented
    EE_PRINTF("[UI] menu exit -> ui=%u beacon=%u\r\n", (unsigned)ui_save, (unsigned)beacon_save);

    // uint8_t i = 4; uint8_t isGreen = true;
    // do { flash_id(1, 125, isGreen); isGreen ^= 0x01; } while(--i);
#endif
}

/*******************************************************************************
 *  Temperature calibration menu
 ******************************************************************************/

void enter_chtk_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_CTK_H;
    //EE_PRINTF("[UI] enter chtk mode t_calibr=%u\r\n", (unsigned)EE.usr.t_calibr);
    aux_count(EE.usr.t_calibr);
#endif
}

void chtk_clickX(void)
{
#ifdef USE_MENU
    EE.usr.t_calibr = clicks_count;
    //EE_PRINTF("[UI] chtk_clickX t_calibr=%u\r\n", (unsigned)EE.usr.t_calibr);
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
#endif
}

/*******************************************************************************
 *  User settings: beacon / backlight / UI select / magic slots
 ******************************************************************************/

void slot_eauxmode(void)
{
#ifdef USE_MENU
    EE.usr.eaux_onoff = !EE.usr.eaux_onoff;
    onoff_indicator(EE.usr.eaux_onoff);
#endif
}

void slot_ipsmode(void)
{
#ifdef USE_MENU
    EE.usr.id_plus_sid = !EE.usr.id_plus_sid;
    onoff_indicator(EE.usr.id_plus_sid);
#endif
}

void enter_bm_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_BM;
    //EE_PRINTF("[UI] enter bm mode=%u\r\n", (unsigned)(EE.aux.beacon_mode + 1));
    aux_count(EE.aux.beacon_mode + 1);
#endif
}

void bm_clickX(void)
{
#ifdef USE_MENU
    EE.aux.beacon_mode = clicks_count - 1;
    //EE_PRINTF("[UI] bm_clickX mode=%u\r\n", (unsigned)EE.aux.beacon_mode);
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
#endif
}

void enter_bt_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_BT;
    //EE_PRINTF("[UI] enter bt type=%u\r\n", (unsigned)((EE.aux.beacon_type >> 1) + 1));
    aux_count((EE.aux.beacon_type >> 1) + 1);
#endif
}

void bt_clickX(void)
{
#ifdef USE_MENU
    EE.aux.beacon_type = (1 << (clicks_count - 1));
    //EE_PRINTF("[UI] bt_clickX type=%u\r\n", (unsigned)EE.aux.beacon_type);
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
#endif
}

void enter_bl_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_BL;
    //EE_PRINTF("[UI] enter bl color=%u\r\n", (unsigned)(EE.aux.bcklight_color + 1));
    aux_count(EE.aux.bcklight_color + 1);
#endif
}

void bl_clickX(void)
{
#ifdef USE_MENU
    EE.aux.bcklight_color = clicks_count - 1;
    //EE_PRINTF("[UI] bl_clickX color=%u\r\n", (unsigned)EE.aux.bcklight_color);
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
#endif
}

void enter_uis_mode(void)
{
#ifdef USE_MENU
    driver_mode = MODE_UIS;
    //EE_PRINTF("[UI] enter uis mode ui=%u\r\n", (unsigned)ui_save);
    aux_count(ui_save);
#endif
}

void uis_clickX(void)
{
#ifdef USE_MENU
    ui_save = clicks_count;
    //EE_PRINTF("[UI] uis_clickX ui=%u\r\n", (unsigned)ui_save);
    aux_count(clicks_count);
    driver_mode = MODE_OFF;
#endif
}


/*******************************************************************************
 *  Magic slots
 ******************************************************************************/

#ifdef USE_MAGIC_SLOTS
void enter_magic_mode(void)
{
    driver_mode = MODE_MAGIC;
    onoff_indicator(true);
}

void exit_magic_mode(void)
{
    driver_mode = MODE_OFF;
    onoff_indicator(true);
}

static void clickX(uint8_t slot)
{
    //EE_PRINTF("[UI] clickX slot=%u value=%u\r\n", (unsigned)slot, (unsigned)clicks_count);
    aux_count(clicks_count);
    ob_SetSlot(slot, clicks_count);
    driver_mode = MODE_MAGIC;
}

#define DEFINE_SLOT_SELECT(n, slot_idx)                     \
    void select_slot##n(void) {                             \
        driver_mode = MODE_SLOT##n;                         \
        EE_PRINTF("[UI] select slot " #n " = %u\r\n",       \
                  (unsigned)ob_GetSlot(slot_idx));          \
        aux_count(ob_GetSlot(slot_idx));                    \
    }                                                       \
    void slot##n##_clickX(void) { clickX(slot_idx); }

DEFINE_SLOT_SELECT(01, 0)
DEFINE_SLOT_SELECT(02, 1)
DEFINE_SLOT_SELECT(03, 2)
DEFINE_SLOT_SELECT(04, 3)
DEFINE_SLOT_SELECT(05, 4)
DEFINE_SLOT_SELECT(06, 5)
DEFINE_SLOT_SELECT(07, 6)
DEFINE_SLOT_SELECT(08, 7)
DEFINE_SLOT_SELECT(09, 8)
DEFINE_SLOT_SELECT(10, 9)
#else
void enter_magic_mode(void) {}
void exit_magic_mode(void) {}
void select_slot01(void) {} void slot01_clickX(void) {}
void select_slot02(void) {} void slot02_clickX(void) {}
void select_slot03(void) {} void slot03_clickX(void) {}
void select_slot04(void) {} void slot04_clickX(void) {}
void select_slot05(void) {} void slot05_clickX(void) {}
void select_slot06(void) {} void slot06_clickX(void) {}
void select_slot07(void) {} void slot07_clickX(void) {}
void select_slot08(void) {} void slot08_clickX(void) {}
void select_slot09(void) {} void slot09_clickX(void) {}
void select_slot10(void) {} void slot10_clickX(void) {}
#endif

/*******************************************************************************
 *  UI2 helpers and actions
 ******************************************************************************/

uint16_t get_stepi(uint8_t index)
{
    return modes[index];
}

uint8_t get_up_mode(uint8_t mode)
{
    uint8_t up_mode = MIN(volt_stop_mode, volt_max_mode);
    return MIN(up_mode, mode);
}

void set_curri(uint8_t cmode)
{
    set_current(get_stepi(cmode), SS_SLOW);
}

static void on_mode(uint8_t m)
{
    ps.use_prev = false;
    on_driver();
    set_curri(m);
}

void on_moon_ui2(void)
{
    mode_current = CMODE_MOON;
    on_mode(mode_current);
}

void on_min_ui2(void)
{
    mode_current = get_up_mode(CMODE_MIN);
    on_mode(mode_current);
}

void on_xmode_ui2(void)
{
    mode_current = get_up_mode(EE.uif.use_memory ? mode_default : EE.default_ui2);
    on_mode(mode_current);
}

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

void mode_down_ui2(void)
{
    if (mode_current > CMODE_MIN)
    {
        ps.use_prev = false;
        set_curri(--mode_current);
    }
}

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

/*******************************************************************************
 *  UI dispatcher
 ******************************************************************************/

void ui_init(void)
{
    memset(&ps, 0, sizeof(ps));
    stop_bit = false;
    need_save = 0;
    direction = DIR_UP;
    mode_current = CMODE_OFF;
    ui_table_idx = EE.uif.ui_index;
    ramp_speed = 2;
    pid_time = 3;
}

void ui_process(void)
{
    uint8_t ui_idx = ui_table_idx;

    /* Tactical mode bypasses FSM events entirely */
    if (ui_idx == 0)
    {
        handle_tactical();
        return;
    }

    /* Long-click threshold per UI */
    if (ui_idx == 1)
        button_set_long_click_ticks(BTN_LONG_CLICK_TICKS);
    else if (ui_idx == 2)
        button_set_long_click_ticks(LONG_PRESS_TIME_UI2);
    else
        button_set_long_click_ticks(VERY_LONG_PRESS_UI2);

    /* Max clicks per UI */
    if (ui_idx == 5 || ui_idx == 6)
        button_set_max_clicks(BTN_MAX_CLICKS_MENU);
    else
        button_set_max_clicks(BTN_MAX_CLICKS);

    /* Direction timeout for UI1 */
    if (!Pressed)
    {
        if (bt_r_time < 0xFF) bt_r_time++;
        if (bt_r_time >= DIRECTION_TIMEOUT_UI1) direction = DIR_UP;
    }
    else
    {
        bt_r_time = 0;
    }

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
                max_step = get_max_step();
                stop_aux_beacon(false);
                if (step_tc >= max_step) direction = DIR_DOWN;
            }
            break;

        case BTN_EVT_RELEASED:
            tick_5s_reset();
            stop_bit = false;
            if (ui_idx == 1) direction ^= 0x01;
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

            /* UI1 hold speed: first hold from OFF -> pause; ON -> user speed */
            if (ui_idx == 1 && prev_mode == MODE_OFF && evt->clicks == 0)
                button_set_repeat_ticks(LONG_HOLD_RAMP_PAUSE_UI1);
            else if (ui_idx == 1 && prev_mode == MODE_ON)
                button_set_repeat_ticks(ramp_speed + 2);
            break;
        }

        case BTN_EVT_CLICK_SEQ:
            tick_5s_reset();
            ui_table_lookup(driver_mode, evt, true);
            break;

        default:
            break;
    }
}
