/**
 * @file button.c
 * @brief Pure FSM for button press pattern recognition
 *
 * Ported from Proteus IID (AVR, 120 Hz) to Proteus003 (CH32V003, 160 Hz).
 *
 * This module reads the button pin via the Pressed macro defined in ui.h
 * and emits btn_evt_t events. It has no knowledge of driver modes, UI
 * actions or hardware other than the button.
 */

#include "button.h"
#include "ui.h"         // For Pressed / Released macros

/*******************************************************************************
 *  Static configuration and state
 ******************************************************************************/

static btn_config_t cfg;

enum {
    ST_IDLE,
    ST_DEBOUNCE,
    ST_PRESSED,
    ST_HOLD,
    ST_HOLD_REPEAT,
    ST_RELEASE_WAIT
};

static uint8_t  state;
static uint8_t  d_time;
static uint8_t  u_time;
static uint8_t  clicks;
static uint8_t  long_mask;
static uint8_t  deb_cnt;
static bool     hold_active;
static bool     prev_pin;
/* true, когда ST_DEBOUNCE фильтрует повторное нажатие внутри sequence:
 * отскок возвращает FSM в ST_RELEASE_WAIT (а не в ST_IDLE), сохраняя
 * накопленные clicks и тайм-аут межкликового окна. */
static bool     deb_seq;

static btn_evt_t pending_evt;
static bool     evt_pending;

/*******************************************************************************
 *  Helpers
 ******************************************************************************/

/**
 * @brief Queue a button event for the main loop to consume.
 * @param type Event type to emit (PRESSED, RELEASED, CLICK_SEQ, HOLD).
 */
static void emit_event(btn_evt_type_t type)
{
    pending_evt.type = type;
    pending_evt.clicks = clicks;
    pending_evt.long_mask = long_mask;
    evt_pending = true;
}

/*******************************************************************************
 *  Public API
 ******************************************************************************/

/**
 * @brief Initialise button FSM with configuration.
 * @param config Pointer to configuration structure (hold/repeat/release ticks,
 *               max clicks, long-click threshold).
 */
void button_init(const btn_config_t *config)
{
    cfg = *config;
    state = ST_IDLE;
    d_time = 0;
    u_time = 0;
    clicks = 0;
    long_mask = 0;
    deb_cnt = 0;
    hold_active = false;
    prev_pin = false;
    evt_pending = false;
    pending_evt.type = BTN_EVT_NONE;
    pending_evt.clicks = 0;
    pending_evt.long_mask = 0;
}

/**
 * @brief Change hold detection threshold at runtime.
 * @param ticks New hold threshold in ticks.
 */
void button_set_hold_ticks(uint8_t ticks)
{
    cfg.hold_ticks = ticks;
}

/**
 * @brief Change hold repeat interval at runtime.
 * @param ticks New repeat interval in ticks.
 */
void button_set_repeat_ticks(uint8_t ticks)
{
    cfg.repeat_ticks = ticks;
}

/**
 * @brief Change long click threshold at runtime.
 * @param ticks New long-click threshold in ticks.
 */
void button_set_long_click_ticks(uint8_t ticks)
{
    cfg.long_click_ticks = ticks;
}

/**
 * @brief Change maximum clicks in a sequence at runtime.
 * @param clicks_cnt New maximum clicks.
 */
void button_set_max_clicks(uint8_t clicks_cnt)
{
    cfg.max_clicks = clicks_cnt;
}

/**
 * @brief Get pending button event.
 * @return Pointer to event structure, or NULL if none pending.
 */
const btn_evt_t* button_get_event(void)
{
    if (!evt_pending) return NULL;
    evt_pending = false;
    return &pending_evt;
}

/**
 * @brief Whether a physical press or click sequence is currently in progress.
 *
 * Used by the AWU strobe loop to stay awake long enough for the FSM to
 * distinguish one click from a multi-click command after a button wake.
 */
bool button_sequence_active(void)
{
    return (state != ST_IDLE) || Pressed;
}

/*******************************************************************************
 *  Main FSM tick -- call once per 160 Hz tick
 ******************************************************************************/

/**
 * @brief Main FSM tick — call once per 160 Hz tick.
 *
 * Reads the button pin, debounces, detects hold/repeat, and emits
 * BTN_EVT_PRESSED / BTN_EVT_RELEASED / BTN_EVT_HOLD / BTN_EVT_CLICK_SEQ
 * events via emit_event().
 */
void button_tick(void)
{
    bool pin = Pressed;
    bool rising = pin && !prev_pin;
    bool falling = !pin && prev_pin;
    prev_pin = pin;

    switch (state)
    {
        /* -- ST_IDLE: waiting for a press -- */
        case ST_IDLE:
            if (rising)
            {
                stop_bit = false;
                deb_cnt = 0;
                state = ST_DEBOUNCE;
            }
            break;

        /* -- ST_DEBOUNCE: filter contact bounce.  PRESSED is emitted only
         * after a clean debounce, so a bounce glitch cannot run the PRESSED
         * handlers and then vanish without a matching release. -- */
        case ST_DEBOUNCE:
            if (falling)
            {
                if (deb_seq)
                {
                    /* Re-press within a sequence bounced off: resume the
                     * inter-click timeout window without losing clicks.
                     * u_time keeps counting only in RELEASE_WAIT, so the
                     * debounce time slightly extends the allowed gap —
                     * same magnitude as the debounce filter itself. */
                    deb_seq = false;
                    state = ST_RELEASE_WAIT;
                }
                else
                {
                    state = ST_IDLE;
                }
            }
            else if (deb_cnt < BTN_DEBOUNCE_TICKS)
            {
                deb_cnt++;
            }
            else
            {
                d_time = 0;
                deb_seq = false;
                state = ST_PRESSED;
                emit_event(BTN_EVT_PRESSED);
            }
            break;

        /* -- ST_PRESSED: confirmed down, watch for hold or release -- */
        case ST_PRESSED:
            if (falling)
            {
                u_time = 0;
                clicks++;
                if (cfg.long_click_ticks && d_time >= cfg.long_click_ticks)
                {
                    /* long_mask is 8 bits: clicks 9..16 (menus) are not
                     * tracked instead of silently truncating the shift. */
                    if (clicks <= 8u)
                        long_mask |= (uint8_t)(1u << (clicks - 1u));
                }
                if (!hold_active)
                {
                    emit_event(BTN_EVT_RELEASED);
                }
                else
                {
                    clicks = 0;
                    long_mask = 0;
                    hold_active = false;
                }
                state = ST_RELEASE_WAIT;
                break;
            }
            d_time++;
            if (cfg.hold_ticks && d_time >= cfg.hold_ticks)
            {
                hold_active = true;
                d_time = 1;
                emit_event(BTN_EVT_HOLD);
                if (cfg.repeat_ticks)
                {
                    state = ST_HOLD;
                }
                else
                {
                    state = ST_HOLD_REPEAT;
                }
            }
            break;

        /* -- ST_HOLD / ST_HOLD_REPEAT: hold timing and repeating hold events -- */
        case ST_HOLD:
        case ST_HOLD_REPEAT:
            if (falling)
            {
                hold_active = false;
                clicks = 0;
                long_mask = 0;
                u_time = 0;
                state = ST_RELEASE_WAIT;
                break;
            }
            d_time++;
            if ((state == ST_HOLD && stop_bit) || (cfg.repeat_ticks && d_time >= cfg.repeat_ticks))
            {
                d_time = 1;
                if (!stop_bit)
                    emit_event(BTN_EVT_HOLD);
                state = ST_HOLD_REPEAT;
            }
            break;

        /* -- ST_RELEASE_WAIT: inter-click timeout or next press -- */
        case ST_RELEASE_WAIT:
            if (rising)
            {
                stop_bit = false;
                deb_cnt = 0;
                deb_seq = true;
                /* Re-presses pass the same debounce gate as the first
                 * press; otherwise contact bounce on click 2..N would
                 * inflate the sequence with phantom presses. */
                state = ST_DEBOUNCE;
                break;
            }
            u_time++;
            if (u_time >= cfg.release_ticks || clicks >= cfg.max_clicks)
            {
                /* A release after HOLD resets clicks to 0; emitting an empty
                 * CLICK_SEQ would dispatch clicks=0 into wildcard entries. */
                if (clicks)
                    emit_event(BTN_EVT_CLICK_SEQ);
                clicks = 0;
                long_mask = 0;
                hold_active = false;
                state = ST_IDLE;
            }
            break;

        default:
            state = ST_IDLE;
            break;
    }
}
