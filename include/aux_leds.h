/**
 * @file aux_leds.h
 * @brief AUX LED controller module (red, green, backlight)
 *
 * Supports two operating modes:
 *   1. Single command (start_aux_cmd) — one blink with given parameters
 *   2. Command array (start_aux_array) — sequence of commands from Flash
 *
 * handle_aux() must be called on each AWU wake (~15 ms for AWU_15MS).
 *
 * 32-bit command format (union AUX_Cmd_u):
 *   [2:0]   device     — channel: 1=RED, 2=GRN, 4=HBL (combinable: 3=RED+GRN)
 *   [3]     cycle      — 0=once, 1=looped (infinite)
 *   [7:4]   count      — repeat count (0..15). Decrements when cycle=0
 *   [10:8]  psc0       — AWU prescaler index for 1st half-period (0..7 → 128..61440)
 *   [16:11] win0       — AWU window for 1st half-period (0..63)
 *   [19:17] psc1       — AWU prescaler index for 2nd half-period
 *   [25:20] win1       — AWU window for 2nd half-period
 *   [28:26] brightness0 — modes[] index for 1st half-period (0..6), or
 *                         CMODE_NOACTION for a direct-step HBL command
 *   [31:29] brightness1 — modes[] index for 2nd half-period (0..6)
 *
 * Half-period time: (prescaler_value × win) / 128000 seconds
 * Parameters set via MAKE_CMD(device, cycle, count, psc0, win0, psc1, win1, brightness0, brightness1)
 */

#ifndef AUX_LEDS_H_
#define AUX_LEDS_H_

#include <stdbool.h>
#include "awu.h"
#include "button_gpio.h"
#include "proteus_config.h"

/**
 * @brief LED channels (bitmasks, combinable: RED|GRN = 3).
 */
typedef enum {
	RED_LED = 0b001,   /**< Red LED */
	GRN_LED	= 0b010,   /**< Green LED */
	HBL_LED	= 0b100    /**< мощный диод */
} LED_Bits_t;

/**
 * @brief Voltage warning levels (determine blink frequency).
 */
typedef enum {
	VWARNING_FAST =	0U,   /**< ~2 s period */
	VWARNING_MEDIUM,        /**< ~4 s period */
	VWARNING_SLOW,          /**< ~8 s period */
	VWARNING_NONE           /**< No warning active */
} WARNING_Fr_t;

/**
 * @brief Indication mode tags (for tracking which mode is running).
 */
typedef enum {
	IDM_OTHER = 0U,    /**< Normal blinking */
	IDM_BEACON,        /**< Beacon mode */
	IDM_VWARNING,      /**< Low voltage warning */
	IDM_STROBE         /**< Strobe mode */
} IDM_Type_t;

/**
 * @brief Half-period FSM steps:
 * PER_INIT → PER_FIRST (ON) → PER_SECOND (OFF) → PER_END → (PER_FIRST or PER_NEXT)
 */
typedef enum {
	PER_INIT = 0U,  /**< Initialisation: load count from command */
	PER_FIRST,      /**< 1st half-period: brightness0, MCU sleeps per0 */
	PER_SECOND,     /**< 2nd half-period: brightness1, MCU sleeps per1 */
	PER_END,        /**< End of cycle: handle counter, launch next */
	PER_NEXT        /**< Command done: need to load next from array */
} PER_Step_t;

/**
 * @brief System commands in command array (is_cmd == 0, bits 5..31 == 0).
 * Handled in handle_aux_array before returning pointer.
 * If a system command is encountered, loop continues (break)
 * until a LED command or AC_STOP is found.
 */
typedef enum {
	AC_STOP = 0U,       /**< Stop array processing (return NULL) */
	AC_REP_ATSTART,     /**< Loop from array start (arr_index = 0) */
	AC_REP_ATSTEPS,     /**< Loop from given step (arr_index = steps) */
	AC_RESERVED         /**< Reserved */
} Array_Cmd_t;

/**
 * @brief AWU prescaler indices for MAKE_CMD macro.
 * Values written to psc0/psc1 command bits.
 * Actual divider = table value (128..61440).
 * Period = (psc_value × win) / 128000 seconds
 */
#define AWU_PSC_128   0   /**< LSI/128  = 1 kHz,  win=1 → 1 ms */
#define AWU_PSC_256   1   /**< LSI/256  = 500 Hz, win=1 → 2 ms */
#define AWU_PSC_512   2   /**< LSI/512  = 250 Hz, win=1 → 4 ms */
#define AWU_PSC_1024  3   /**< LSI/1024 = 125 Hz, win=1 → 8 ms */
#define AWU_PSC_2048  4   /**< LSI/2048 = 62.5 Hz, win=1 → 16 ms */
#define AWU_PSC_4096  5   /**< LSI/4096 = 31.25 Hz, win=1 → 32 ms */
#define AWU_PSC_10240 6   /**< LSI/10240 ≈ 12.5 Hz, win=1 → 80 ms */
#define AWU_PSC_61440 7   /**< LSI/61440 ≈ 2.1 Hz, win=1 → 480 ms */

/**
 * @brief Actual AWU divider by prescaler index (non-linear table).
 * Index 0..7 → 128, 256, 512, 1024, 2048, 4096, 10240, 61440.
 */
#define AWU_PSC_VALUE(psc_idx) \
	((psc_idx) == 0 ? 128UL   : \
	 (psc_idx) == 1 ? 256UL   : \
	 (psc_idx) == 2 ? 512UL   : \
	 (psc_idx) == 3 ? 1024UL  : \
	 (psc_idx) == 4 ? 2048UL  : \
	 (psc_idx) == 5 ? 4096UL  : \
	 (psc_idx) == 6 ? 10240UL : 61440UL)

/**
 * @brief Compute half-period in milliseconds from prescaler index and window.
 * Formula: time_ms = (psc_value × win) / 128
 */
#define AWU_PERIOD_MS(psc_idx, win) ((AWU_PSC_VALUE(psc_idx) * (uint32_t)(win)) / 128)

/**
 * @brief 32-bit LED command word.
 *
 * Two representations via union:
 *   aux      — LED command (brightness, timing)
 *   cmd      — System command (AC_STOP, AC_REP_ATSTART, AC_REP_ATSTEPS)
 *   aux_cmd  — Direct 32-bit word access (for Flash storage)
 *
 * If bits 5..31 == 0 → system command (cmd.command).
 * Otherwise → LED command (aux.device, aux.psc0, etc.)
 */
union AUX_Cmd_u {
	/** @brief LED command fields */
	struct __attribute__((packed)) {
		uint32_t device      : 3;  /**< Bits 0..2:   channel (1=RED, 2=GRN, 4=HBL, combos: 3=RED+GRN) */
		uint32_t cycle       : 1;  /**< Bit 3:       0=once, 1=loop forever */
		uint32_t count       : 4;  /**< Bits 4..7:   repeat count (0..15). Decrements when cycle=0 */
		uint32_t psc0        : 3;  /**< Bits 8..10:  AWU prescaler index for 1st half-period (0..7) */
		uint32_t win0        : 6;  /**< Bits 11..16: AWU window for 1st half-period (0..63) */
		uint32_t psc1        : 3;  /**< Bits 17..19: AWU prescaler index for 2nd half-period */
		uint32_t win1        : 6;  /**< Bits 20..25: AWU window for 2nd half-period */
		uint32_t brightness0 : 3;  /**< Bits 26..28: modes[] index of 1st half-period */
		uint32_t brightness1 : 3;  /**< Bits 29..31: modes[] index of 2nd half-period */
	} aux;

	/** @brief System command fields */
	struct __attribute__((packed)) {
		uint32_t command  : 2;   /**< Bits 0..1:  system command (0=STOP, 1=REP_ATSTART, 2=REP_ATSTEPS) */
		uint32_t steps    : 3;   /**< Bits 2..4:  loop step (0..7) for REP_ATSTEPS */
		uint32_t is_cmd  : 27;   /**< Bits 5..31: if ==0 → system command, else LED command */
	} cmd;

	uint32_t aux_cmd; /**< Raw 32-bit word access (for Flash I/O) */
};

/** @brief Compile-time layout check */
_Static_assert(sizeof(union AUX_Cmd_u) == 4, "AUX_Cmd_u must be 32 bits");

/**
 * @brief AUX handler control structure (32 bits).
 *
 * Holds current FSM stage, brightness, array index, repeat counter.
 * Single instance: aux_ctl
 */
typedef struct {
	uint32_t enable             : 1;  /**< Bit 0:      1=processing active, 0=disabled */
	uint32_t period             : 3;  /**< Bits 1..3:  current FSM stage (PER_INIT..PER_NEXT) */
	uint32_t current_brightness : 3;  /**< Bits 4..6:  current brightness (opt: change only on switch) */
	uint32_t idmode             : 2;  /**< Bits 7..8:  indication mode (IDM_OTHER/BEACON/VWARNING/STROBE) */
	uint32_t arr_index          : 4;  /**< Bits 9..12: current command array index (0..15) */
	uint32_t work_count         : 4;  /**< Bits 13..16: remaining repeats of current command (0..15) */
	uint32_t reserved           : 15; /**< Bits 17..31: reserved */
} __attribute__((packed, aligned(4))) AUX_Ctr_t;

/**
 * @brief Beacon mode period options.
 */
typedef enum {
	BCON_1S	= 0U,
	BCON_2S,
	BCON_4S,
	BCON_8S,
} Beacon_Mode_t;

/** @brief Number of selectable AWU/DAC strobe profiles. */
#define STROBE_COUNT 6u

/**
 * @brief Backlight type (off/on).
 */
typedef enum {
	BCK_TYPE_OFF = 0U,
	BCK_TYPE_ON
} BCK_Type_t;

/**
 * @brief Backlight colour.
 */
typedef enum {
	BCK_COLOR_RED =	0U,
	BCK_COLOR_GRN
} BCK_Color_t;

/** @brief Single command buffer for start_aux_cmd(). */
extern union AUX_Cmd_u aux_cmd;
/** @brief Unified AUX handler state structure. */
extern AUX_Ctr_t aux_ctl;
/** @brief Pointer to current command (single or from array). */
extern union AUX_Cmd_u *p_aux;

/** @brief Current voltage warning level. */
extern uint8_t vwarning;
/** @brief Previous voltage warning level (for change detection). */
extern uint8_t vwarning_old;

/** @brief true = array processing active. */
extern bool EnableArrayProcessing;
/** @brief Pointer to current command array. */
extern const uint32_t *current_buf;
/** @brief true when beacon operates in double-flash mode due to low battery (<3.1V). */
extern bool beacon_low_voltage;

/* ===== API functions ===== */

/**
 * @brief Check if AUX is running in a specific mode.
 * @param mode Mode to check (IDM_BEACON, IDM_VWARNING, IDM_STROBE, IDM_OTHER).
 * @return true if active in that mode.
 */
bool is_aux_mode(IDM_Type_t mode);

/**
 * @brief Check whether any AUX command currently owns the AWU.
 * @return true while an AUX command or command array is active.
 */
bool aux_is_active(void);

/**
 * @brief Whether the active AUX command needs the DAC/I2C output path.
 *
 * Used after an AWU/STANDBY wake to restore the peripheral before a HBL or
 * strobe state changes its output.
 */
bool aux_needs_dac(void);

/**
 * @brief Full stop: disable handler and turn off all LEDs.
 * @param stop_lsi If true, stop LSI/AWU.
 */
void off_aux_cmd(bool stop_lsi);

/**
 * @brief Stop AUX if running in specified mode.
 * @param mode Mode to match for stopping.
 * @param stop_lsi If true, also stop LSI/AWU.
 */
static inline void stop_aux_if(IDM_Type_t mode, bool stop_lsi) {
	if (is_aux_mode(mode))
		off_aux_cmd(stop_lsi);
}

/**
 * @brief Start an energy-efficient DAC strobe profile.
 *
 * A profile may be a single packed command or an array of commands.  The
 * common AUX handler processes both forms from AWU wake-ups without changing
 * step_user or invoking normal driver power management on its OFF phase.
 * @param index Strobe profile in range 0..STROBE_COUNT-1.
 */
void start_aux_strobe(uint8_t index);

/**
 * @brief Start beacon mode from EEPROM settings.
 */
void start_aux_beacon(void);

/**
 * @brief Check whether the beacon is at the start of its repetition cycle.
 * @return true on the cycle-initial AWU wake before the first flash.
 */
bool beacon_is_cycle_start(void);

/**
 * @brief Battery supervision during beacon AWU wakes (cutoff <2.7V, 3.1V/3.2V hysteresis).
 */
void beacon_battery_check(void);

/**
 * @brief Refresh button backlight state according to EE.aux settings.
 */
void aux_refresh_backlight(void);

/**
 * @brief Indicate a number via AUX LEDs.
 * @param count 0 = one red flash, otherwise N green flashes.
 */
void aux_count(uint8_t count);

/**
 * @brief One green flash (power-on indication).
 */
void aux_on(void);

/**
 * @brief One red flash (power-off indication).
 */
void aux_off(void);

/**
 * @brief Three short red flashes (error indication).
 * @param delay Initial delay, or AWU_IMMEDIATELY to start without waiting.
 */
void aux_err(awu_tick_t delay);

/**
 * @brief One short red flash (lock-mode press indication).
 */
void aux_dang(void);

/**
 * @brief Main AUX processing loop (called on each AWU wake).
 * @return true if commands still active (AWU will be restarted), false if all done.
 */
bool handle_aux(void);

/**
 * @brief Макрос формирования 32-битной AUX-команды
 *
 * Упаковывает параметры моргания LED в 32-битное слово для хранения в Flash.
 * Результат можно использовать в start_aux_cmd() или в массивах команд.
 *
 * Время полупериода = (psc_значение × win) / 128000 секунд
 *   psc=0 (128),  win=1 → 1мс;    win=50 → 50мс;    win=63 → 63мс
 *   psc=5 (4096), win=1 → 32мс;   win=16 → 512мс;   win=63 → 2.06с
 *   psc=7 (61440), win=1 → 480мс; win=16 → 7.68с
 *
 * @param device       канал (RED_LED=1, GRN_LED=2, HBL_LED=4, RED|GRN=3)
 * @param cycle        0=однократно, 1=зациклено (бесконечно)
 * @param count        количество повторов (0..15). При cycle=0 уменьшается
 * @param psc0         индекс предделителя 1-го полупериода (AWU_PSC_128..AWU_PSC_61440)
 * @param win0         значение окна 1-го полупериода (0..63)
 * @param psc1         индекс предделителя 2-го полупериода
 * @param win1         значение окна 2-го полупериода (0..63)
 * @param brightness0  индекс modes[] 1-го полупериода (0=выкл, 1..6=уровень)
 *                     или CMODE_NOACTION для прямого шага HBL
 * @param brightness1  индекс modes[] 2-го полупериода (0=выкл, 1..6=уровень)
 */
#define MAKE_CMD(device, cycle, count, psc0, win0, psc1, win1, brightness0, brightness1) \
	((uint32_t)((device) & 0x7)              | \
	 ((uint32_t)((cycle) & 0x1) << 3)        | \
	 ((uint32_t)((count) & 0xF) << 4)        | \
	 ((uint32_t)((psc0) & 0x7) << 8)         | \
	 ((uint32_t)((win0) & 0x3F) << 11)       | \
	 ((uint32_t)((psc1) & 0x7) << 17)        | \
	 ((uint32_t)((win1) & 0x3F) << 20)       | \
	 ((uint32_t)((brightness0) & 0x7) << 26) | \
	 ((uint32_t)((brightness1) & 0x7) << 29))

/**
 * @brief Round an AWU window for a requested period.
 *
 * The AWU period is psc_value * window / 128 milliseconds.  The rounded
 * window is clamped to the 6-bit hardware range.  A zero request remains
 * zero because it is used by command encodings that must not wait.
 */
#define AWU_MS_WIN_FOR(ms, psc_value) \
	((ms) == 0U ? 0U : \
	 ((((uint32_t)(ms) * 128UL + ((psc_value) / 2UL)) / (psc_value)) < 1UL ? 1U : \
	  (((uint32_t)(ms) * 128UL + ((psc_value) / 2UL)) / (psc_value)) > 63UL ? 63U : \
	  (((uint32_t)(ms) * 128UL + ((psc_value) / 2UL)) / (psc_value))))

/**
 * @brief Compare period error without floating-point arithmetic.
 * @return Absolute error multiplied by 128 (milliseconds * 128).
 */
#define AWU_MS_ERROR(ms, psc_value) \
	((uint32_t)(((uint32_t)(ms) * 128UL) >= \
	 ((uint32_t)(psc_value) * AWU_MS_WIN_FOR((ms), (psc_value))) ? \
	 ((uint32_t)(ms) * 128UL) - \
	 ((uint32_t)(psc_value) * AWU_MS_WIN_FOR((ms), (psc_value))) : \
	 ((uint32_t)(psc_value) * AWU_MS_WIN_FOR((ms), (psc_value))) - \
	 ((uint32_t)(ms) * 128UL)))

/**
 * @brief Select an AWU prescaler for a millisecond period.
 *
 * AWU windows are rounded to the nearest integer.  The limits below select
 * the finest practical prescaler over each monotonic time range while
 * keeping this macro small enough for the embedded compiler to expand.
 * This is a monotonic approximation, not a full search for the globally
 * smallest absolute error: the finer prescaler remains selected until its
 * 6-bit window reaches the useful end of its range.  Ties at a boundary
 * select the smaller prescaler.
 *
 * Supported nominal range is 0..30240 ms.  Requests above 30240 ms use
 * AWU_PSC_61440 with window 63 and therefore saturate at 30240 ms.
 */
#define AWU_MS_PSC(ms) \
	((ms) <= 63UL    ? AWU_PSC_128   : \
	 (ms) <= 127UL   ? AWU_PSC_256   : \
	 (ms) <= 254UL   ? AWU_PSC_512   : \
	 (ms) <= 508UL   ? AWU_PSC_1024  : \
	 (ms) <= 1016UL  ? AWU_PSC_2048  : \
	 (ms) <= 2048UL  ? AWU_PSC_4096  : \
	 (ms) <= 5160UL  ? AWU_PSC_10240 : \
	                  AWU_PSC_61440)

/**
 * @brief Return the rounded AWU window for a millisecond period.
 *
 * The selected window is clamped to the hardware range 1..63 for a
 * non-zero request.  Consequently, periods above the maximum AWU range
 * saturate at the largest representable period.
 */
#define AWU_MS_WIN(ms) \
	AWU_MS_WIN_FOR((ms), AWU_PSC_VALUE(AWU_MS_PSC(ms)))

/**
 * @brief MAKE_CMD с миллисекундами вместо psc/win
 *
 * @param device       канал (RED_LED=1, GRN_LED=2, HBL_LED=4, комбинации через |)
 * @param cycle        0=однократно, 1=зациклено
 * @param count        количество повторов (0..15)
 * @param ms0          длительность 1-го полупериода в миллисекундах
 * @param ms1          длительность 2-го полупериода в миллисекундах
 * @param brightness0  индекс modes[] 1-го полупериода (0=выкл, 1..6=уровень)
 * @param brightness1  индекс modes[] 2-го полупериода (0=выкл, 1..6=уровень)
 *
 * Для каждого полупериода выбирается монотонный диапазон предделителя,
 * затем окно округляется к ближайшему целому. Номинальный диапазон
 * длительности — 0..30240 мс; значения больше 30240 мс насыщаются на
 * максимуме AWU. Реальное время дополнительно зависит от частоты LSI.
 */
#define MAKE_CMD_MS(device, cycle, count, ms0, ms1, brightness0, brightness1) \
    MAKE_CMD(device, cycle, count, \
             AWU_MS_PSC(ms0), AWU_MS_WIN(ms0), \
             AWU_MS_PSC(ms1), AWU_MS_WIN(ms1), \
             brightness0, brightness1)

#define INFO_RED  MAKE_CMD_MS(RED_LED, 0, 1, 448, 896, 1, 0)  /* 1 красная ~448/896мс */
#define INFO_GRN  MAKE_CMD_MS(GRN_LED, 0, 2, 224, 112, 1, 0)  /* 2 зелёных ~224/112мс */
#define ERRO_RED  MAKE_CMD_MS(RED_LED, 0, 3, 15, 90, 1, 0)    /* 3 красных ~15/90мс */
#define DANG_RED  MAKE_CMD_MS(RED_LED, 0, 1, 15, 90, 1, 0)    /* 1 красная ~15/90мс */

#endif /* AUX_LEDS_H_ */
