#ifndef UI_H_
#define UI_H_

/*
 * ui.h -- Пользовательский интерфейс (UI) и обработка кнопки
 *
 * Портированная версия:
 *   - FSM кнопки в button.c/h
 *   - Табличная диспетчеризация в ui_table.c/h
 *   - Action functions реализованы в ui.c
 */

#include <stdbool.h>
#include <stdint.h>

#include "ch32v00x.h"
#include "debug.h"
#include "soft_start.h"
#include "button.h"

struct ee_data;

/*******************************************************************************
 *  PWM / step globals
 ******************************************************************************/

extern uint16_t step_default;
extern uint16_t step_user;
extern uint16_t step_tc;
extern uint16_t max_step;
extern uint8_t  need_save;

/*******************************************************************************
 *  Turbo-toggle memory
 ******************************************************************************/

typedef struct {
	uint16_t start;
	uint16_t pr_step;
	uint16_t use_prev;
} PSTEP_t;

/*******************************************************************************
 *  Defaults
 ******************************************************************************/

#define BASE_MOON_LEVEL 5
#define DEFAULT_STEP    128     // Шаг для UI1 при начальной инициализации
#define DEFAULT_MODE    CMODE_MID // Режим для UI2 при начальной инициализации

/*******************************************************************************
 *  Button pin (PC3)
 ******************************************************************************/

#define KEY_PORT    GPIOC
#define KEY_PIN     GPIO_Pin_3

#define Released    (KEY_PORT->INDR & KEY_PIN)
#define Pressed     (!Released)

/*******************************************************************************
 *  Driver modes
 ******************************************************************************/

typedef enum {
    MODE_BLOCK      = 0U,
    MODE_BLOCK0,
    MODE_BLOCK_C,
    MODE_OFF,
    MODE_ON,
    MODE_STROB,

    MODE_RAMP_SPEED = 10U,
    MODE_MOON_LEVEL,
    MODE_PIP_TIME,

    MODE_CTK_A,
    MODE_CTK_H,
    MODE_MAGIC,
    MODE_UIS,
    MODE_BM,
    MODE_BT,
    MODE_BL,

    MODE_SLOT01,
    MODE_SLOT02,
    MODE_SLOT03,
    MODE_SLOT04,
    MODE_SLOT05,
    MODE_SLOT06,
    MODE_SLOT07,
    MODE_SLOT08,
    MODE_SLOT09,
    MODE_SLOT10
} DRIVER_Mode_t;

extern DRIVER_Mode_t driver_mode;

/*******************************************************************************
 *  UI2 discrete brightness modes
 ******************************************************************************/

typedef enum {
    CMODE_OFF = 0U,
    CMODE_MOON,
    CMODE_MIN,
    CMODE_MID0,
    CMODE_MID,
    CMODE_MAX,
    CMODE_TURBO,
    CMODE_NOACTION
} CURRENT_Modes_t;

extern uint8_t mode_current;
extern uint8_t mode_default;

/*******************************************************************************
 *  UI timing constants (recalculated for 160 Hz)
 ******************************************************************************/

#define RELEASE_TIMEOUT_UI1         40
#define LONG_HOLD_UI1               100
#define LONG_HOLD_RAMP_PAUSE_UI1    80
#define DIRECTION_TIMEOUT_UI1       80
#define COMM_MAX_LEN_UI1            7

#define ENG_MENU_IND                5
#define USR_MENU_IND                6

#define RELEASE_TIMEOUT_UI4         RELEASE_TIMEOUT_UI1
#define VERY_LONG_PRESS_UI4         LONG_HOLD_UI1
#define COMM_MAX_LEN_UI4            31

#define RELEASE_TIMEOUT_UI2         RELEASE_TIMEOUT_UI1
#define LONG_PRESS_TIME_UI2         40
#define VERY_LONG_PRESS_UI2         93
#define COMM_MAX_LEN_UI2            COMM_MAX_LEN_UI1

#define MODES_QTY                   7
#define SLEEP_DELAY                 108     // kept for compatibility; real sleep via tick_5s
#define SLEEP_DELAY_BAT             2500

/*******************************************************************************
 *  Ramp direction
 ******************************************************************************/

typedef enum {
    DIR_DOWN = 0U,
    DIR_UP
} RUMP_Dir_t;

#define MAX_STEP    511
#define POW_THRD    120

/*******************************************************************************
 *  Core UI API
 ******************************************************************************/

void ui_init(void);
void ui_process(void);

void off_driver_false(void);
void step_up_ui1(void);
void step_down_ui1(void);
void on_rumpup_ui1(void);
void on_rumpdown_ui1(void);
void on_xmode_ui1(void);
void on_turbo_ui1(void);

void set_current_ll(uint16_t step_target, SoftStartSpeed_t ss_speed);
void set_current(uint16_t step_target, SoftStartSpeed_t ss_speed);
uint16_t get_step_tc(uint16_t step);
void out_set_stc(uint16_t step);

void ON_POWER(void);
void OFF_POWER(void);

void ee_defaults(void);

/*******************************************************************************
 *  Brightness helpers
 ******************************************************************************/

uint16_t get_min_level(uint16_t step);
uint16_t getBright_CRT_9to12Bit(uint16_t val);
uint16_t get_stepi(uint8_t index);
uint8_t get_up_mode(uint8_t mode);
void set_curri(uint8_t cmode);

/*******************************************************************************
 *  Slot / block / strobe action functions (used by main_op[] in ui_table.c)
 ******************************************************************************/

void exit_magic_mode(void);
void slot_empty(void);
void slot_batt_ind(void);
void slot_strobe(void);
void slot_block_beacon(void);
void slot_block(void);
void slot_tactical(void);
void slot_backlight(void);
void slot_beacon(void);
void slot_eauxmode(void);
void slot_ipsmode(void);
void slot_memory(void);

void up_strobe(void);
void back_strobe(void);
void off_strobe(void);

void unblock_drv(void);
void off_driver_false(void);

/*******************************************************************************
 *  UI1 / menu action functions
 ******************************************************************************/

void on_xmode_ui1(void);
void on_rumpup_ui1(void);
void on_rumpdown_ui1(void);
void on_turbo_ui1(void);
void calibration(void);

void enter_rs_mode(void);
void enter_ml_mode(void);
void enter_pt_mode(void);
void save_def_ui1(void);
void save_def_ui2(void);
void rs_clickX(void);
void ml_clickX(void);
void pt_clickX(void);
void menu_exit(void);

void enter_chtk_mode(void);
void chtk_clickX(void);

void enter_bm_mode(void);
void bm_clickX(void);
void enter_bt_mode(void);
void bt_clickX(void);
void enter_bl_mode(void);
void bl_clickX(void);

void enter_magic_mode(void);

void select_slot01(void);
void slot01_clickX(void);
void select_slot02(void);
void slot02_clickX(void);
void select_slot03(void);
void slot03_clickX(void);
void select_slot04(void);
void slot04_clickX(void);
void select_slot05(void);
void slot05_clickX(void);
void select_slot06(void);
void slot06_clickX(void);
void select_slot07(void);
void slot07_clickX(void);
void select_slot08(void);
void slot08_clickX(void);
void select_slot09(void);
void slot09_clickX(void);
void select_slot10(void);
void slot10_clickX(void);

void mode_up_ui2(void);
void on_turbo_ui2(void);
void on_min_ui2(void);
void on_moon_ui2(void);
void on_xmode_ui2(void);
void mode_down_ui2(void);
void step_down_ui2(void);
void enter_uis_mode(void);
void uis_clickX(void);
void reset2def(void);

/*******************************************************************************
 *  Shared state variables
 ******************************************************************************/

extern uint8_t   VLPT;
extern RUMP_Dir_t direction;
extern uint16_t  step_tactic;
extern uint8_t   need_save;
extern bool      stop_bit;
extern PSTEP_t   ps;

extern uint8_t   clicks_count;      // from last btn_evt_t
extern uint8_t   ui_table_idx;      // runtime UI/menu selector (separate from ee.uif.ui_index)
extern uint8_t   ramp_speed;        // replaces ee.eng.rump_speed
extern uint8_t   pid_time;          // replaces ee.eng.pid_time

uint8_t on_driver(void);
void off_driver(uint8_t reset_step, SoftStartSpeed_t ss_speed);
void off_auto_repeat(void);

#endif /* UI_H_ */
