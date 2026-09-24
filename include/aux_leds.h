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
 * USE_IND_RGB=1: поле cycle отсутствует, его бит отдан device:
 *   [3:0]   device     — channel: 1=RED, 2=GRN, 4=HBL, 8=BLUE
 * Остальные поля на прежних местах, поэтому команды с cycle=0 кодируются
 * байт-идентично дефолтному формату. Зацикливание (бывший cycle=1)
 * оформляется массивом {cmd, AC_REP_ATSTART}. Как и раньше, LED-команда
 * обязана иметь ненулевые биты >= 5 (тайминги/яркость): полностью нулевые
 * тайминги попадают в пространство системных команд (AUX_IS_SYS_CMD).
 *
 * Half-period time: (prescaler_value × win) / 128000 seconds
 * Parameters set via MAKE_CMD(device, cycle, count, psc0, win0, psc1, win1, brightness0, brightness1)
 *
 * The command word is serialised into Flash, therefore it is NEVER decoded
 * through C bitfields (their layout is ABI-dependent).  All encoding and
 * decoding goes through the explicit AUX_*_MASK/AUX_*_SHIFT macros and
 * AUX_xxx() accessors; _Static_assert checks pin the layout at compile time.
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
	HBL_LED	= 0b100,   /**< мощный диод */
#if USE_IND_RGB
	BLUE_LED = 0b1000  /**< Blue LED (PC6) */
#endif
} LED_Bits_t;

#if USE_IND_RGB
/** @brief Все GPIO-каналы индикатора (без HBL). */
#define AUX_GPIO_LEDS	((LED_Bits_t)(RED_LED | GRN_LED | BLUE_LED))
#else
#define AUX_GPIO_LEDS	((LED_Bits_t)(RED_LED | GRN_LED))
#endif

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
 * @brief 32-bit LED command word.
 *
 * The word is serialised into Flash arrays, so it MUST NOT be decoded via
 * C bitfields: bitfield member layout is implementation-defined (ABI- and
 * compiler-dependent).  All fields are accessed exclusively through the
 * explicit mask/shift macros below (AUX_DEVICE(), AUX_PSC0(), ...), which
 * makes the encoding independent of bitfield layout and byte order.
 *
 * If bits 5..31 == 0 → system command (AUX_IS_SYS_CMD(), decode with
 * AUX_SYS_CMD_OP()/AUX_SYS_CMD_STEPS(), build with MAKE_SYS_CMD()).
 * Otherwise → LED command (AUX_DEVICE(), AUX_PSC0(), etc.)
 */
union AUX_Cmd_u {
	uint32_t aux_cmd; /**< Raw 32-bit word access (for Flash I/O) */
};

/** @brief Compile-time layout check */
_Static_assert(sizeof(union AUX_Cmd_u) == 4, "AUX_Cmd_u must be 32 bits");

/* ===== Command field layout: single source of truth =====
 * Bit positions match the table in the file header comment.  The static
 * asserts below pin the layout at compile time: no overlap, no gaps. */

#if USE_IND_RGB
#define AUX_DEVICE_SHIFT	0u   /**< Bits 0..3:   channel mask (+BLUE_LED) */
#else
#define AUX_DEVICE_SHIFT	0u   /**< Bits 0..2:   channel mask */
#define AUX_CYCLE_SHIFT		3u   /**< Bit 3:       0=once, 1=looped */
#endif
#define AUX_COUNT_SHIFT		4u   /**< Bits 4..7:   repeat count (0..15) */
#define AUX_PSC0_SHIFT		8u   /**< Bits 8..10:  AWU prescaler, 1st half-period */
#define AUX_WIN0_SHIFT		11u  /**< Bits 11..16: AWU window, 1st half-period */
#define AUX_PSC1_SHIFT		17u  /**< Bits 17..19: AWU prescaler, 2nd half-period */
#define AUX_WIN1_SHIFT		20u  /**< Bits 20..25: AWU window, 2nd half-period */
#define AUX_BR0_SHIFT		26u  /**< Bits 26..28: brightness index, 1st half-period */
#define AUX_BR1_SHIFT		29u  /**< Bits 29..31: brightness index, 2nd half-period */

#if USE_IND_RGB
#define AUX_DEVICE_MASK		(0x0FUL << AUX_DEVICE_SHIFT)
#else
#define AUX_DEVICE_MASK		(0x07UL << AUX_DEVICE_SHIFT)
#define AUX_CYCLE_MASK		(0x01UL << AUX_CYCLE_SHIFT)
#endif
#define AUX_COUNT_MASK		(0x0FUL << AUX_COUNT_SHIFT)
#define AUX_PSC0_MASK		(0x07UL << AUX_PSC0_SHIFT)
#define AUX_WIN0_MASK		(0x3FUL << AUX_WIN0_SHIFT)
#define AUX_PSC1_MASK		(0x07UL << AUX_PSC1_SHIFT)
#define AUX_WIN1_MASK		(0x3FUL << AUX_WIN1_SHIFT)
#define AUX_BR0_MASK		(0x07UL << AUX_BR0_SHIFT)
#define AUX_BR1_MASK		(0x07UL << AUX_BR1_SHIFT)

#if USE_IND_RGB
_Static_assert((AUX_DEVICE_MASK & (AUX_COUNT_MASK | AUX_PSC0_MASK |
		AUX_WIN0_MASK | AUX_PSC1_MASK | AUX_WIN1_MASK |
		AUX_BR0_MASK | AUX_BR1_MASK)) == 0u &&
		(AUX_COUNT_MASK & (AUX_PSC0_MASK | AUX_WIN0_MASK | AUX_PSC1_MASK |
		AUX_WIN1_MASK | AUX_BR0_MASK | AUX_BR1_MASK)) == 0u &&
		(AUX_PSC0_MASK & (AUX_WIN0_MASK | AUX_PSC1_MASK | AUX_WIN1_MASK |
		AUX_BR0_MASK | AUX_BR1_MASK)) == 0u &&
		(AUX_WIN0_MASK & (AUX_PSC1_MASK | AUX_WIN1_MASK |
		AUX_BR0_MASK | AUX_BR1_MASK)) == 0u &&
		(AUX_PSC1_MASK & (AUX_WIN1_MASK | AUX_BR0_MASK | AUX_BR1_MASK)) == 0u &&
		(AUX_WIN1_MASK & (AUX_BR0_MASK | AUX_BR1_MASK)) == 0u,
		"AUX command fields overlap");
_Static_assert((AUX_DEVICE_MASK | AUX_COUNT_MASK |
		AUX_PSC0_MASK | AUX_WIN0_MASK | AUX_PSC1_MASK | AUX_WIN1_MASK |
		AUX_BR0_MASK | AUX_BR1_MASK) == 0xFFFFFFFFUL,
		"AUX command fields leave a gap");
#else
_Static_assert((AUX_DEVICE_MASK & (AUX_CYCLE_MASK | AUX_COUNT_MASK |
		AUX_PSC0_MASK | AUX_WIN0_MASK | AUX_PSC1_MASK | AUX_WIN1_MASK |
		AUX_BR0_MASK | AUX_BR1_MASK)) == 0u &&
		(AUX_CYCLE_MASK & (AUX_COUNT_MASK | AUX_PSC0_MASK | AUX_WIN0_MASK |
		AUX_PSC1_MASK | AUX_WIN1_MASK | AUX_BR0_MASK | AUX_BR1_MASK)) == 0u &&
		(AUX_COUNT_MASK & (AUX_PSC0_MASK | AUX_WIN0_MASK | AUX_PSC1_MASK |
		AUX_WIN1_MASK | AUX_BR0_MASK | AUX_BR1_MASK)) == 0u &&
		(AUX_PSC0_MASK & (AUX_WIN0_MASK | AUX_PSC1_MASK | AUX_WIN1_MASK |
		AUX_BR0_MASK | AUX_BR1_MASK)) == 0u &&
		(AUX_WIN0_MASK & (AUX_PSC1_MASK | AUX_WIN1_MASK |
		AUX_BR0_MASK | AUX_BR1_MASK)) == 0u &&
		(AUX_PSC1_MASK & (AUX_WIN1_MASK | AUX_BR0_MASK | AUX_BR1_MASK)) == 0u &&
		(AUX_WIN1_MASK & (AUX_BR0_MASK | AUX_BR1_MASK)) == 0u,
		"AUX command fields overlap");
_Static_assert((AUX_DEVICE_MASK | AUX_CYCLE_MASK | AUX_COUNT_MASK |
		AUX_PSC0_MASK | AUX_WIN0_MASK | AUX_PSC1_MASK | AUX_WIN1_MASK |
		AUX_BR0_MASK | AUX_BR1_MASK) == 0xFFFFFFFFUL,
		"AUX command fields leave a gap");
#endif

#define AUX_FIELD(word, mask, shift) \
	((uint8_t)(((uint32_t)(word) & (uint32_t)(mask)) >> (shift)))

#define AUX_DEVICE(word)	AUX_FIELD((word), AUX_DEVICE_MASK, AUX_DEVICE_SHIFT)
#if USE_IND_RGB
/* Поля cycle в RGB-формате нет: зацикливание — массивом с AC_REP_ATSTART.
 * Акцессор оставлен константой 0, чтобы handle_aux_cmd не ветвился. */
#define AUX_CYCLE(word)		0u
#else
#define AUX_CYCLE(word)		AUX_FIELD((word), AUX_CYCLE_MASK, AUX_CYCLE_SHIFT)
#endif
#define AUX_COUNT(word)		AUX_FIELD((word), AUX_COUNT_MASK, AUX_COUNT_SHIFT)
#define AUX_PSC0(word)		AUX_FIELD((word), AUX_PSC0_MASK, AUX_PSC0_SHIFT)
#define AUX_WIN0(word)		AUX_FIELD((word), AUX_WIN0_MASK, AUX_WIN0_SHIFT)
#define AUX_PSC1(word)		AUX_FIELD((word), AUX_PSC1_MASK, AUX_PSC1_SHIFT)
#define AUX_WIN1(word)		AUX_FIELD((word), AUX_WIN1_MASK, AUX_WIN1_SHIFT)
#define AUX_BR0(word)		AUX_FIELD((word), AUX_BR0_MASK, AUX_BR0_SHIFT)
#define AUX_BR1(word)		AUX_FIELD((word), AUX_BR1_MASK, AUX_BR1_SHIFT)

/* ===== System commands (bits 5..31 == 0): opcode [1:0], steps [4:2] ===== */

#define AUX_IS_SYS_CMD(word)	(((uint32_t)(word) & 0xFFFFFFE0UL) == 0)
#define AUX_SYS_CMD_OP(word) \
	((Array_Cmd_t)AUX_FIELD((word), 0x03UL, 0u))
#define AUX_SYS_CMD_STEPS(word)	AUX_FIELD((word), 0x1CUL, 2u)

#define MAKE_SYS_CMD(op, steps) \
	((uint32_t)((op) & 0x3u) | ((uint32_t)((steps) & 0x7u) << 2))

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

/* AUX_Ctr_t is an internal (never serialised) state container, bitfield
 * layout is safe here; only the total size is pinned. */
_Static_assert(sizeof(AUX_Ctr_t) == 4, "AUX_Ctr_t must fit in 32 bits");

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
#define STROBE_COUNT 8u

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
	BCK_COLOR_GRN,
#if USE_IND_RGB
	BCK_COLOR_BLUE          /**< Только RGB: третий цвет подсветки (PC6) */
#endif
} BCK_Color_t;

#if USE_IND_RGB
/** @brief Индексы типа маяка в EE.aux (RGB-раскладка): значение = клики меню - 1. */
typedef enum {
	BT_RED = 0U,   /**< RED_LED */
	BT_GRN,        /**< GRN_LED */
	BT_BLUE,       /**< BLUE_LED */
	BT_HBL         /**< HBL_LED */
} Beacon_Type_t;
#endif

/** @brief Single command buffer for start_aux_cmd(). */
extern union AUX_Cmd_u aux_cmd;
/** @brief Unified AUX handler state structure. */
extern AUX_Ctr_t aux_ctl;
/** @brief Pointer to current command (single or from array). */
extern union AUX_Cmd_u *p_aux;

/** @brief Current voltage warning level. */
extern uint8_t vwarning;

/** @brief true = array processing active. */
extern bool EnableArrayProcessing;
/** @brief Pointer to current command array. */
extern const uint32_t *current_buf;
/** @brief true when beacon operates in double-flash mode due to low battery (<3.1V). */
extern bool beacon_low_voltage;
/** @brief true = exaux warning indication fired this battery session; beacon double-flashes after off. */
extern bool vwarning_fired;

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

#if USE_EX_AUX
/**
 * @brief Start extended aux voltage warning blink pattern.
 * @param ex_mode Warning level (VWARNING_FAST=double flash ~2s,
 *                VWARNING_MEDIUM/SLOW=single flash 4s/8s).
 */
void start_exaux_voltage(WARNING_Fr_t ex_mode);
#endif

/**
 * @brief Check whether the beacon is at the start of its repetition cycle.
 * @return true on the cycle-initial AWU wake before the first flash.
 */
bool beacon_is_cycle_start(void);

/**
 * @brief Battery supervision during beacon AWU wakes (cutoff <2.7V, 3.1V/3.2V hysteresis).
 */
void beacon_battery_check(void);

#if USE_BEACON_ADC_GUARD
/**
 * @brief True while the beacon is latched off by a persistent ADC failure.
 *
 * Three consecutive failed beacon_battery_check() calls latch the beacon
 * off; start_aux_beacon() refuses to restart it until the latch clears.
 */
bool beacon_adc_dead(void);

/**
 * @brief Clear the ADC-failure beacon latch.
 *
 * Called from the button-wake path after the first valid ADC sample:
 * user presence plus a proven-live ADC allow the beacon to resume on the
 * next OFF-sleep (if still enabled in EE).
 */
void beacon_adc_latch_reset(void);
#endif /* USE_BEACON_ADC_GUARD */

/**
 * @brief Refresh button backlight state according to EE.aux settings.
 */
void aux_refresh_backlight(void);

/**
 * @brief Гашение каналов с учётом настройки подсветки (backlight).
 *
 * Если подсветка включена и её цвет совпадает с каналом, канал переводится
 * в тусклый режим (on_*_l), иначе полностью гасится (off_*). Биты HBL_LED
 * в текущей реализации игнорируются.
 *
 * @param channel Битовая маска каналов (RED_LED|GRN_LED|HBL_LED).
 */
void smart_off_leds(LED_Bits_t channel);

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

#if USE_AUX_ERR
/**
 * @brief Three short red flashes (error indication).
 * @param delay Initial delay, or AWU_IMMEDIATELY to start without waiting.
 * @note Runs only while the exaux flag (UIF_EAUX_ONOFF in EE.uif, user menu) is set.
 */
void aux_err(awu_tick_t delay);

/**
 * @brief No-argument form of aux_err(AWU_IMMEDIATELY).
 * @note Noinline call form: keeps the constant inside one wrapper instead
 *       of materialising it at every immediate call site (Flash saving).
 */
void aux_err_now(void);

/**
 * @brief One short red flash (lock-mode press indication).
 * @note Runs only while the exaux flag (UIF_EAUX_ONOFF in EE.uif, user menu) is set.
 */
void aux_dang(void);
#else
#define aux_err(delay) ((void)0)
#define aux_err_now() ((void)0)
#define aux_dang() ((void)0)
#endif

/* ===== Magic slots indication (unconditional, no exaux flag) =====
 * Color dictionary:
 *   orange (RED+GRN together) = magic-mode enter/exit cue,
 *   red    = slot number flashes and rejection of invalid input (always,
 *            see aux_reject),
 *   green  = value (slot function / confirmation). */

/**
 * @brief Indicate the picked magic slot: slot_no red flashes, then op green.
 *
 * Two-part non-blocking sequence (AWU-FSM) via a RAM buffer.  The ~320 ms
 * OFF pause after the last red flash separates the slot-number group
 * from the value group.
 * @param slot_no Slot number 1..10 (clamped to 1..15, AUX_COUNT is 4 bits).
 * @param op      Function number in the slot 1..12 (clamped to 1..15).
 */
void aux_magic_slot_info(uint8_t slot_no, uint8_t op);

/**
 * @brief Orange confirmation (RED+GRN): magic-mode enter/exit cue.
 * @param long_pulse false — 2 short flashes ~150 ms each (enter slot picker);
 *                   true — 1 long ~500 ms (exit magic mode, menu_exit style).
 */
void aux_magic_cue(bool long_pulse);

/**
 * @brief Rejection of invalid input: three short red flashes.
 *
 * Unconditional (unlike aux_err(), which is gated by the exaux flag):
 * used for all user-menu sub-modes, calibration() and the magic slot
 * picker/assigner.
 */
void aux_reject(void);

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
#if USE_IND_RGB
/* RGB-формат: поля cycle нет (бит 3 отдан device). Параметр cycle оставлен
 * в сигнатуре ради мест вызова, но обязан быть 0 — зацикливание
 * оформляется массивом {cmd, AC_REP_ATSTART}. */
#define MAKE_CMD(device, cycle, count, psc0, win0, psc1, win1, brightness0, brightness1) \
	((uint32_t)((device) & 0xFu) << AUX_DEVICE_SHIFT        | \
	 ((uint32_t)((count) & 0xFu) << AUX_COUNT_SHIFT)        | \
	 ((uint32_t)((psc0) & 0x7u) << AUX_PSC0_SHIFT)          | \
	 ((uint32_t)((win0) & 0x3Fu) << AUX_WIN0_SHIFT)         | \
	 ((uint32_t)((psc1) & 0x7u) << AUX_PSC1_SHIFT)          | \
	 ((uint32_t)((win1) & 0x3Fu) << AUX_WIN1_SHIFT)         | \
	 ((uint32_t)((brightness0) & 0x7u) << AUX_BR0_SHIFT)    | \
	 ((uint32_t)((brightness1) & 0x7u) << AUX_BR1_SHIFT))
#else
#define MAKE_CMD(device, cycle, count, psc0, win0, psc1, win1, brightness0, brightness1) \
	((uint32_t)((device) & 0x7u) << AUX_DEVICE_SHIFT        | \
	 ((uint32_t)((cycle) & 0x1u) << AUX_CYCLE_SHIFT)        | \
	 ((uint32_t)((count) & 0xFu) << AUX_COUNT_SHIFT)        | \
	 ((uint32_t)((psc0) & 0x7u) << AUX_PSC0_SHIFT)          | \
	 ((uint32_t)((win0) & 0x3Fu) << AUX_WIN0_SHIFT)         | \
	 ((uint32_t)((psc1) & 0x7u) << AUX_PSC1_SHIFT)          | \
	 ((uint32_t)((win1) & 0x3Fu) << AUX_WIN1_SHIFT)         | \
	 ((uint32_t)((brightness0) & 0x7u) << AUX_BR0_SHIFT)    | \
	 ((uint32_t)((brightness1) & 0x7u) << AUX_BR1_SHIFT))
#endif

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
 * @brief Select an AWU prescaler for a millisecond period.
 *
 * AWU windows are rounded to the nearest integer.  The limits below select
 * the finest practical prescaler over each monotonic time range while
 * keeping this macro small enough for the embedded compiler to expand.
 * Each boundary is the exact maximum period representable by that
 * prescaler's window 63 (psc*63/128 ms), so the 6-bit window is never
 * clamped from 64 down to 63 — the clamp shortened the period by up to
 * ~1.5 % (audit C4; boundaries fixed 2048→2016, 5160→5040).
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
	 (ms) <= 2016UL  ? AWU_PSC_4096  : \
	 (ms) <= 5040UL  ? AWU_PSC_10240 : \
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

/* Pin the C4 boundaries: window 63 must fit exactly at each edge, and the
 * next millisecond above it must switch to the coarser prescaler. */
_Static_assert(AWU_MS_PSC(2016) == AWU_PSC_4096 && AWU_MS_PSC(2017) == AWU_PSC_10240 &&
		AWU_MS_PSC(5040) == AWU_PSC_10240 && AWU_MS_PSC(5041) == AWU_PSC_61440 &&
		AWU_MS_WIN(2016) == 63UL && AWU_MS_WIN(5040) == 63UL,
		"AWU_MS_PSC window-clamp boundaries changed");

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
#define ERRO_RED  MAKE_CMD_MS(RED_LED, 0, 3, 15, 60, 1, 0)    /* 3 красных ~15/60мс */
#define DANG_RED  MAKE_CMD_MS(RED_LED, 0, 1, 15, 60, 1, 0)    /* 1 красная ~15/60мс */

/* Freeze the documented encodings of the known commands (regression guard
 * against accidental changes of MAKE_CMD_MS/AWU rounding constants). */
_Static_assert(INFO_RED == 0x0789C311UL && INFO_GRN == 0x0783C222UL &&
		ERRO_RED == 0x07C07831UL && DANG_RED == 0x07C07811UL,
		"Known AUX command encodings changed");

/* Round-trip layout test: all fields at their maximum value must fill the
 * whole 32-bit word and decode back identically, pinning the bit format
 * independently of any bitfield ABI. */
#if USE_IND_RGB
#define AUX_LAYOUT_TEST_CMD MAKE_CMD(0xFu, 0u, 15u, 7u, 63u, 7u, 63u, 7u, 7u)
_Static_assert(AUX_LAYOUT_TEST_CMD == 0xFFFFFFFFUL,
		"MAKE_CMD layout does not tile the 32-bit word");
_Static_assert(AUX_DEVICE(AUX_LAYOUT_TEST_CMD) == 0xFu &&
		AUX_CYCLE(AUX_LAYOUT_TEST_CMD) == 0u &&
		AUX_COUNT(AUX_LAYOUT_TEST_CMD) == 15u &&
		AUX_PSC0(AUX_LAYOUT_TEST_CMD) == 7u &&
		AUX_WIN0(AUX_LAYOUT_TEST_CMD) == 63u &&
		AUX_PSC1(AUX_LAYOUT_TEST_CMD) == 7u &&
		AUX_WIN1(AUX_LAYOUT_TEST_CMD) == 63u &&
		AUX_BR0(AUX_LAYOUT_TEST_CMD) == 7u &&
		AUX_BR1(AUX_LAYOUT_TEST_CMD) == 7u,
		"AUX command encode/decode round-trip failed");
#else
#define AUX_LAYOUT_TEST_CMD MAKE_CMD(0x7u, 1u, 15u, 7u, 63u, 7u, 63u, 7u, 7u)
_Static_assert(AUX_LAYOUT_TEST_CMD == 0xFFFFFFFFUL,
		"MAKE_CMD layout does not tile the 32-bit word");
_Static_assert(AUX_DEVICE(AUX_LAYOUT_TEST_CMD) == 0x7u &&
		AUX_CYCLE(AUX_LAYOUT_TEST_CMD) == 1u &&
		AUX_COUNT(AUX_LAYOUT_TEST_CMD) == 15u &&
		AUX_PSC0(AUX_LAYOUT_TEST_CMD) == 7u &&
		AUX_WIN0(AUX_LAYOUT_TEST_CMD) == 63u &&
		AUX_PSC1(AUX_LAYOUT_TEST_CMD) == 7u &&
		AUX_WIN1(AUX_LAYOUT_TEST_CMD) == 63u &&
		AUX_BR0(AUX_LAYOUT_TEST_CMD) == 7u &&
		AUX_BR1(AUX_LAYOUT_TEST_CMD) == 7u,
		"AUX command encode/decode round-trip failed");
#endif
#undef AUX_LAYOUT_TEST_CMD

#endif /* AUX_LEDS_H_ */
