/**
 * @file button.h
 * @brief Button FSM for press pattern recognition
 *
 * Ported from Proteus84 (AVR, 120 Hz) to Proteus003 (CH32V003, 160 Hz).
 * This module reads the button pin via the Pressed macro defined in ui.h
 * and emits btn_evt_t events. It has no knowledge of driver modes, UI
 * actions or hardware other than the button.
 */

#ifndef BUTTON_H_
#define BUTTON_H_

#include <stdint.h>
#include <stdbool.h>

/**
 * @defgroup button_timing Button FSM timing constants
 *
 * Proteus84 used 120 Hz ticks. Proteus003 calls button_tick() at 160 Hz,
 * so all tick counts are scaled by 4/3 and rounded.
 * @{
 */
#define BTN_TICK_HZ            160     /**< Main loop tick rate used for button FSM */
#define BTN_DEBOUNCE_TICKS     3       /**< ~18.8 ms debounce time */
#define BTN_HOLD_TICKS         100     /**< ~625 ms hold detection */
#define BTN_REPEAT_TICKS       4       /**< ~25 ms hold repeat interval */
#define BTN_RELEASE_TICKS      40      /**< ~250 ms inter-click timeout */
#define BTN_MAX_CLICKS         7       /**< Max clicks for UI1/UI2 */
#define BTN_MAX_CLICKS_MENU    31      /**< Max clicks for engineering/user menus */
#define BTN_LONG_CLICK_TICKS   40      /**< ~250 ms threshold for "long" click */
/** @} */

/**
 * @defgroup button_events FSM event types
 *
 * The button FSM emits these events only. It knows nothing about driver
 * modes or UI actions.
 * @{
 */
/** @brief Button event type enumeration. */
typedef enum {
    BTN_EVT_NONE = 0,            /**< No event */
    BTN_EVT_PRESSED,             /**< Button just pressed (debounced rising edge) */
    BTN_EVT_RELEASED,            /**< Button released, not held */
    BTN_EVT_CLICK_SEQ,           /**< Click sequence complete (release timeout or max clicks) */
    BTN_EVT_HOLD,                /**< Hold detected (or repeat tick) */
} btn_evt_type_t;
/** @} */

/** @brief Button event structure. */
typedef struct {
    btn_evt_type_t type;    /**< Which event occurred */
    uint8_t        clicks;  /**< Completed clicks in current sequence */
    uint8_t        long_mask; /**< Bitmask of "long" clicks (bit N = click N was long) */
} btn_evt_t;

/** @brief Runtime configurable button parameters. */
typedef struct {
    uint8_t hold_ticks;         /**< Ticks before hold event */
    uint8_t repeat_ticks;       /**< Ticks between hold repeat events */
    uint8_t release_ticks;      /**< Inter-click release timeout */
    uint8_t max_clicks;         /**< Maximum clicks in a sequence */
    uint8_t long_click_ticks;   /**< Threshold for "long" click classification */
} btn_config_t;

/* ===== Public API ===== */

/**
 * @brief Initialise button FSM with configuration.
 * @param config Pointer to configuration structure.
 */
void button_init(const btn_config_t *config);

/**
 * @brief Main FSM tick — call once per 160 Hz tick.
 */
void button_tick(void);

/**
 * @brief Change hold detection threshold at runtime.
 * @param ticks New hold threshold in ticks.
 */
void button_set_hold_ticks(uint8_t ticks);

/**
 * @brief Change hold repeat interval at runtime.
 * @param ticks New repeat interval in ticks.
 */
void button_set_repeat_ticks(uint8_t ticks);

/**
 * @brief Change long click threshold at runtime.
 * @param ticks New long-click threshold in ticks.
 */
void button_set_long_click_ticks(uint8_t ticks);

/**
 * @brief Change maximum clicks in a sequence at runtime.
 * @param clicks New maximum clicks.
 */
void button_set_max_clicks(uint8_t clicks);

/**
 * @brief Get pending button event.
 * @return Pointer to event structure, or NULL if none pending.
 */
const btn_evt_t* button_get_event(void);

/**
 * @brief Whether a physical press or click sequence is currently in progress.
 *
 * Used by the AWU strobe loop to stay awake long enough for the FSM to
 * distinguish one click from a multi-click command after a button wake.
 */
bool button_sequence_active(void);

/**
 * @brief Reset click counter and long mask.
 */
void button_reset_click_count(void);

#endif /* BUTTON_H_ */
