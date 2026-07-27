#ifndef BUTTON_H_
#define BUTTON_H_

#include <stdint.h>
#include <stdbool.h>

/*******************************************************************************
 *  Button FSM timing
 *
 *  Proteus84 used 120 Hz ticks. Proteus003 calls button_tick() at 160 Hz,
 *  so all tick counts are scaled by 4/3 and rounded.
 ******************************************************************************/

#define BTN_TICK_HZ            160     // Main loop tick rate used for button FSM

#define BTN_DEBOUNCE_TICKS     3       // ~18.8 ms
#define BTN_HOLD_TICKS         100     // ~625 ms
#define BTN_REPEAT_TICKS       4      // ~25 ms
#define BTN_RELEASE_TICKS      40      // ~250 ms inter-click timeout
#define BTN_MAX_CLICKS         7      // Max clicks for UI1/UI2
#define BTN_MAX_CLICKS_MENU    31      // Max clicks for engineering/user menus
#define BTN_LONG_CLICK_TICKS   40      // ~250 ms threshold for "long" click

/*******************************************************************************
 *  FSM event types
 *
 *  The button FSM emits these events only. It knows nothing about driver modes
 *  or UI actions.
 ******************************************************************************/

typedef enum {
    BTN_EVT_NONE = 0,
    BTN_EVT_PRESSED,        // Button just pressed (debounced rising edge)
    BTN_EVT_RELEASED,       // Button released, not held
    BTN_EVT_CLICK_SEQ,      // Click sequence complete (release timeout or max clicks)
    BTN_EVT_HOLD,           // Hold detected (or repeat tick)
} btn_evt_type_t;

typedef struct {
    btn_evt_type_t type;    // Which event occurred
    uint8_t        clicks;  // Completed clicks in current sequence
    uint8_t        long_mask; // Bitmask of "long" clicks (bit N = click N was long)
} btn_evt_t;

/*******************************************************************************
 *  Runtime configurable parameters
 ******************************************************************************/

typedef struct {
    uint8_t hold_ticks;
    uint8_t repeat_ticks;
    uint8_t release_ticks;
    uint8_t max_clicks;
    uint8_t long_click_ticks;
} btn_config_t;

/*******************************************************************************
 *  Public API
 ******************************************************************************/

void button_init(const btn_config_t *config);
void button_tick(void);
void button_set_hold_ticks(uint8_t ticks);
void button_set_repeat_ticks(uint8_t ticks);
void button_set_long_click_ticks(uint8_t ticks);
void button_set_max_clicks(uint8_t clicks);
const btn_evt_t* button_get_event(void);
void button_reset_click_count(void);

#endif /* BUTTON_H_ */
