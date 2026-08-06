/**
 * @file ui.h
 * @brief Пользовательский интерфейс (UI) и обработка кнопки
 *
 * Портированная версия:
 *   - FSM кнопки в button.c/h
 *   - Табличная диспетчеризация в ui_table.c/h
 *   - Action functions реализованы в ui.c
 */

#ifndef INC_UI_H_
#define INC_UI_H_

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

/** @brief Default step value for UI1 at initialisation. */
extern uint16_t step_default;

/** @brief Current user-requested step (brightness) value. */
extern uint16_t step_user;

/** @brief Step value limited by thermal controller. */
extern uint16_t step_tc;

/** @brief Maximum allowed step (511 for 9-bit DAC). */
extern uint16_t max_step;

/** @brief Flag indicating EEPROM save is pending. */
extern uint8_t need_save;

/*******************************************************************************
 *  Turbo-toggle memory
 ******************************************************************************/

/** @brief State for double-click turbo toggle feature. */
typedef struct {
    uint16_t start;     /**< Step at which turbo was entered. */
    uint16_t pr_step;   /**< Previous step before turbo. */
    uint16_t use_prev;  /**< Flag: restore pr_step on exit. */
} PSTEP_t;

/*******************************************************************************
 *  Defaults
 ******************************************************************************/

/** @brief Base moonlight level (PWM value). */
#define BASE_MOON_LEVEL 5

/** @brief Default step for UI1 at initialisation. */
#define DEFAULT_STEP    128

/** @brief Default mode for UI2 at initialisation. */
#define DEFAULT_MODE    CMODE_MID

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

/**
 * @brief Driver operating modes.
 *
 * Values 0-9: main driver states.
 * Values 10+: configuration sub-modes and magic slots.
 */
typedef enum {
    MODE_BLOCK      = 0U,   /**< Driver blocked (hard fault). */
    MODE_BLOCK0,            /**< Soft block (waiting for release). */
    MODE_BLOCK_C,           /**< Block from click sequence. */
    MODE_OFF,               /**< Driver off. */
    MODE_ON,                /**< Driver on (normal operation). */
    MODE_STROB,             /**< Strobe mode. */

    MODE_MOON_LEVEL = 10U,  /**< Config: moonlight level. */

    MODE_CTK_A,             /**< Config: thermal calibration A. */
    MODE_CTK_H,             /**< Config: thermal calibration H. */
    MODE_MAGIC,             /**< Magic slots mode. */
    MODE_UIS,               /**< User interface select. */
    MODE_BM,                /**< Beacon mode config. */
    MODE_BT,                /**< Beacon time config. */
    MODE_BL,                /**< Backlight config. */

    MODE_SLOT01,            /**< Magic slot 1. */
    MODE_SLOT02,            /**< Magic slot 2. */
    MODE_SLOT03,            /**< Magic slot 3. */
    MODE_SLOT04,            /**< Magic slot 4. */
    MODE_SLOT05,            /**< Magic slot 5. */
    MODE_SLOT06,            /**< Magic slot 6. */
    MODE_SLOT07,            /**< Magic slot 7. */
    MODE_SLOT08,            /**< Magic slot 8. */
    MODE_SLOT09,            /**< Magic slot 9. */
    MODE_SLOT10             /**< Magic slot 10. */
} DRIVER_Mode_t;

/** @brief Current driver mode. */
extern DRIVER_Mode_t driver_mode;

/*******************************************************************************
 *  UI2 discrete brightness modes
 ******************************************************************************/

/** @brief Discrete brightness mode indices for UI2. */
typedef enum {
    CMODE_OFF     = 0U,   /**< Off. */
    CMODE_MOON,           /**< Moonlight. */
    CMODE_MIN,            /**< Minimum. */
    CMODE_MID0,           /**< Low-medium. */
    CMODE_MID,            /**< Medium. */
    CMODE_MAX,            /**< Maximum. */
    CMODE_TURBO,          /**< Turbo (time-limited). */
    CMODE_NOACTION        /**< No action / placeholder. */
} CURRENT_Modes_t;

/** @brief Currently active discrete mode (UI2). */
extern uint8_t mode_current;

/** @brief Default discrete mode at power-on (UI2). */
extern uint8_t mode_default;

/*******************************************************************************
 *  UI timing constants (recalculated for 160 Hz)
 ******************************************************************************/

#define RELEASE_TIMEOUT_UI1         40
#define LONG_HOLD_UI1               100
#define LONG_HOLD_RAMP_PAUSE_UI1    80
#define FASTEST_RAMP_REPEAT_UI1     2
#define DIRECTION_TIMEOUT_UI1       80
#define COMM_MAX_LEN_UI1            7

#define ENG_MENU_IND                5
#define USR_MENU_IND                6

/* Порог выбора меню при подаче питания с нажатой кнопкой: инженерное меню
 * доступно только при питании выше 4.5 В (стенд/USB), ниже — меню пользователя.
 * Как в оригинале Proteus IID (ENG_VOLTAGE). */
#define ENG_VOLTAGE_MV              4500u

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

/** @brief Direction of brightness ramping. */
typedef enum {
    DIR_DOWN = 0U,
    DIR_UP
} RUMP_Dir_t;

/** @brief Indicator colour indices. */
typedef enum {
    IND_RED = 0,
    IND_GRN
} ind_clr_t;

#define MAX_STEP    511
#define POW_THRD    120

/*******************************************************************************
 *  Core UI API
 ******************************************************************************/

/**
 * @brief Initialise UI subsystem (state, defaults, power).
 */
void ui_init(void);

/**
 * @brief Main UI dispatcher — call from main loop at 160 Hz.
 *
 * Reads button events, executes dispatch table actions, handles ramping.
 */
void ui_process(void);

/**
 * @brief Helper: set off_driver reset flag to false.
 */
void off_driver_false(void);

/**
 * @brief Increase brightness by one step (UI1 ramping).
 */
void step_up_ui1(void);

/**
 * @brief Decrease brightness by one step (UI1 ramping).
 */
void step_down_ui1(void);

/**
 * @brief Start ramp-up with current direction and speed.
 */
void on_rumpup_ui1(void);

/**
 * @brief Start ramp-down with current direction and speed.
 */
void on_rumpdown_ui1(void);

/**
 * @brief Toggle between UI1/UI2 or enter config mode.
 */
void on_xmode_ui1(void);

/**
 * @brief Enter turbo mode (momentary max brightness).
 */
void on_turbo_ui1(void);

/**
 * @brief Low-level DAC current setter without soft-start.
 * @param step_target Target 9-bit step (0..511).
 * @param ss_speed    Soft-start speed (SS_NO, SS_SLOW, SS_FAST).
 */
void set_current_ll(uint16_t step_target, SoftStartSpeed_t ss_speed);

/**
 * @brief High-level current setter with thermal limiting.
 * @param step_target Target 9-bit step (0..511).
 * @param ss_speed    Soft-start speed (SS_NO, SS_SLOW, SS_FAST).
 */
void set_current(uint16_t step_target, SoftStartSpeed_t ss_speed);

/**
 * @brief Get thermal-limited step from raw step.
 * @param step Raw step value.
 * @return Thermally limited step.
 */
uint16_t get_step_tc(uint16_t step);

/**
 * @brief Write step value to DAC via soft-start.
 * @param step 9-bit step value (0..511).
 */
void out_set_stc(uint16_t step);

/**
 * @brief Power up DAC and I2C.
 */
void ON_POWER(void) __attribute__((noinline));

/**
 * @brief Power down DAC and I2C.
 */
void OFF_POWER(void);

/**
 * @brief Load factory default settings into EEPROM cache.
 */
void ee_defaults(void);

/*******************************************************************************
 *  Brightness helpers
 ******************************************************************************/

/**
 * @brief Get minimum DAC level for a given step (prevents flicker at low PWM).
 * @param step 9-bit step value.
 * @return Minimum 12-bit DAC value.
 */
uint16_t get_min_level(uint16_t step);

/**
 * @brief Convert 9-bit perceptual step to 12-bit DAC value (cubic curve).
 * @param val 9-bit step (0..511).
 * @return 12-bit DAC value (0..4095).
 */
uint16_t getBright_CRT_9to12Bit(uint16_t val);

/**
 * @brief Get 9-bit step value for a discrete UI2 mode index.
 * @param index Mode index (0..MODES_QTY-1).
 * @return 9-bit step value.
 */
uint16_t get_stepi(uint8_t index);

/**
 * @brief Set current step from discrete mode index.
 * @param cmode Discrete mode index (CMODE_*).
 */
void set_curri(uint8_t cmode);

/*******************************************************************************
 *  Slot / block / strobe action functions (used by main_op[] in ui_table.c)
 ******************************************************************************/

/**
 * @brief Exit magic slots mode and return to normal operation.
 */
void exit_magic_mode(void);

/**
 * @brief Empty slot action (no operation).
 */
void slot_empty(void);

/**
 * @brief Show battery indication via AUX LEDs.
 */
void slot_batt_ind(void);

/**
 * @brief Start strobe mode.
 */
void slot_strobe(void);

/**
 * @brief Toggle beacon mode on/off.
 */
void slot_block_beacon(void);

/**
 * @brief Enter tactical block mode (momentary on).
 */
void slot_block(void);

/**
 * @brief Enter tactical mode (momentary max).
 */
void slot_tactical(void);

/**
 * @brief Toggle backlight on/off.
 */
void slot_backlight(void);

/**
 * @brief Toggle beacon mode.
 */
void slot_beacon(void);

/**
 * @brief Toggle extra auxiliary mode.
 */
void slot_eauxmode(void);

/**
 * @brief Toggle IPS mode.
 */
void slot_ipsmode(void);

/**
 * @brief Save current mode as default for UI1/UI2.
 */
void slot_memory(void);

/**
 * @brief Cycle to next strobe pattern.
 */
void up_strobe(void);

/**
 * @brief Cycle to previous strobe pattern.
 */
void back_strobe(void);

/**
 * @brief Turn off strobe and exit strobe mode.
 */
void off_strobe(void);

/**
 * @brief Unblock driver after block condition.
 */
void unblock_drv(void);

/**
 * @brief Duplicate of off_driver_false for dispatch table.
 */
void off_driver_false(void);

/*******************************************************************************
 *  UI1 / menu action functions
 ******************************************************************************/

/**
 * @brief Toggle between UI1/UI2 or enter config (duplicate for dispatch).
 */
void on_xmode_ui1(void);

/**
 * @brief Start ramp-up (duplicate for dispatch).
 */
void on_rumpup_ui1(void);

/**
 * @brief Start ramp-down (duplicate for dispatch).
 */
void on_rumpdown_ui1(void);

/**
 * @brief Enter turbo mode (duplicate for dispatch).
 */
void on_turbo_ui1(void);

/**
 * @brief Run voltage/temperature calibration (engineering menu).
 */
void calibration(void);

/**
 * @brief Enter moonlight level config mode.
 */
void enter_ml_mode(void);

/**
 * @brief Save current step as UI1 default.
 */
void save_def_ui1(void);

/**
 * @brief Save current mode as UI2 default.
 */
void save_def_ui2(void);

/**
 * @brief Handle click sequence in moonlight menu.
 */
void ml_clickX(void);

/**
 * @brief Exit current menu mode.
 */
void menu_exit(void);

/**
 * @brief Enter thermal calibration (engineering) mode.
 */
void enter_chtk_mode(void);

/**
 * @brief Handle click sequence in thermal calibration menu.
 */
void chtk_clickX(void);

/**
 * @brief Enter beacon mode config.
 */
void enter_bm_mode(void);

/**
 * @brief Handle click sequence in beacon mode menu.
 */
void bm_clickX(void);

/**
 * @brief Enter beacon time config.
 */
void enter_bt_mode(void);

/**
 * @brief Handle click sequence in beacon time menu.
 */
void bt_clickX(void);

/**
 * @brief Enter backlight config.
 */
void enter_bl_mode(void);

/**
 * @brief Handle click sequence in backlight menu.
 */
void bl_clickX(void);

/**
 * @brief Enter magic slots mode.
 */
void enter_magic_mode(void);

/**
 * @brief Select magic slot based on click count.
 */
void select_slot(void);

/**
 * @brief Generic magic slot click handler (slot index from clicks).
 */
void slotN_clickX(void);

/*******************************************************************************
 *  UI2 action functions
 ******************************************************************************/

/**
 * @brief Increase discrete mode (UI2).
 */
void mode_up_ui2(void);

/**
 * @brief Enter turbo mode (UI2).
 */
void on_turbo_ui2(void);

/**
 * @brief Set minimum mode (UI2).
 */
void on_min_ui2(void);

/**
 * @brief Set moonlight mode (UI2).
 */
void on_moon_ui2(void);

/**
 * @brief Toggle xmode (UI2).
 */
void on_xmode_ui2(void);

/**
 * @brief Decrease discrete mode (UI2).
 */
void mode_down_ui2(void);

/**
 * @brief Step down brightness (UI2).
 */
void step_down_ui2(void);

/**
 * @brief Enter UI select mode.
 */
void enter_uis_mode(void);

/**
 * @brief Handle click sequence in UI select menu.
 */
void uis_clickX(void);

/**
 * @brief Reset to factory defaults.
 */
void reset2def(void);

/*******************************************************************************
 *  Shared state variables
 ******************************************************************************/

/** @brief Very long press timeout (ticks). */
extern uint8_t VLPT;

/** @brief Current ramp direction. */
extern RUMP_Dir_t direction;

/** @brief Tactical mode step storage. */
extern uint16_t step_tactic;

/** @brief EEPROM save pending flag. */
extern uint8_t need_save;

/** @brief Stop flag for auto-repeat. */
extern bool stop_bit;

/** @brief Turbo toggle state. */
extern PSTEP_t ps;

/** @brief Number of clicks in last button event. */
extern uint8_t clicks_count;

/** @brief Runtime UI/menu selector (1=UI1, 2=UI2, 3=UI4, etc.). */
extern uint8_t ui_table_idx;

/**
 * @brief Enter engineering/user menu at power-up when the button is held.
 *
 * Runs once during boot (main calls it only if the button is already pressed
 * at startup). Keeps the button pressed for the debounce window, then selects
 * the menu from the supply voltage: above ENG_VOLTAGE_MV (4.5 V) — engineering
 * menu, otherwise — user menu. Mirrors original Proteus IID enter_menu().
 */
void enter_menu_at_boot(void);

/** @brief Session-only mute of the AUX beacon while a menu is active.
 *
 * Set at menu entry, cleared on menu_exit(). RAM flag, so it resets on reset
 * and never touches EE.aux.beacon_on — a power-cycle inside the menu cannot
 * permanently disable the beacon. */
extern bool menu_beacon_mute;

/**
 * @brief Turn on driver and initialise soft-start.
 * @return 1 if driver was actually turned on, 0 if already on or blocked.
 */
uint8_t on_driver(void);

/**
 * @brief Turn off driver with optional step reset.
 * @param reset_step If non-zero, reset step_user to default.
 * @param ss_speed   Soft-start speed for ramp-down.
 */
void off_driver(uint8_t reset_step, SoftStartSpeed_t ss_speed);

/**
 * @brief Cancel any pending auto-repeat.
 */
void off_auto_repeat(void);

#endif /* INC_UI_H_ */
