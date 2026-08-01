/**
 * @file button.c
 * @brief Pure FSM for button press pattern recognition
 *
 * Ported from Proteus84 (AVR, 120 Hz) to Proteus003 (CH32V003, 160 Hz).
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

static btn_evt_t pending_evt;
static bool     evt_pending;

/*******************************************************************************
 *  Helpers
 ******************************************************************************/

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

void button_set_hold_ticks(uint8_t ticks)
{
    cfg.hold_ticks = ticks;
}

void button_set_repeat_ticks(uint8_t ticks)
{
    cfg.repeat_ticks = ticks;
}

void button_set_long_click_ticks(uint8_t ticks)
{
    cfg.long_click_ticks = ticks;
}

void button_set_max_clicks(uint8_t clicks_cnt)
{
    cfg.max_clicks = clicks_cnt;
}

const btn_evt_t* button_get_event(void)
{
    if (!evt_pending) return NULL;
    evt_pending = false;
    return &pending_evt;
}

bool button_sequence_active(void)
{
    return (state != ST_IDLE) || Pressed;
}

void button_reset_click_count(void)
{
    clicks = 0;
    long_mask = 0;
}

/*******************************************************************************
 *  Main FSM tick -- call once per 160 Hz tick
 ******************************************************************************/

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
                deb_cnt = 0;
                emit_event(BTN_EVT_PRESSED);
                state = ST_DEBOUNCE;
            }
            break;

        /* -- ST_DEBOUNCE: filter contact bounce -- */
        case ST_DEBOUNCE:
            if (falling)
            {
                state = ST_IDLE;
            }
            else if (deb_cnt < BTN_DEBOUNCE_TICKS)
            {
                deb_cnt++;
            }
            else
            {
                d_time = 0;
                state = ST_PRESSED;
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
                    long_mask |= (1 << (clicks - 1));
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

        /* -- ST_HOLD: first hold tick emitted, waiting repeat interval -- */
        case ST_HOLD:
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
            if (cfg.repeat_ticks && d_time >= cfg.repeat_ticks)
            {
                d_time = 1;
                emit_event(BTN_EVT_HOLD);
                state = ST_HOLD_REPEAT;
            }
            break;

        /* -- ST_HOLD_REPEAT: repeating hold events -- */
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
            if (cfg.repeat_ticks && d_time >= cfg.repeat_ticks)
            {
                d_time = 1;
                emit_event(BTN_EVT_HOLD);
            }
            break;

        /* -- ST_RELEASE_WAIT: inter-click timeout or next press -- */
        case ST_RELEASE_WAIT:
            if (rising)
            {
                d_time = 1;
                state = ST_PRESSED;
                break;
            }
            u_time++;
            if (u_time >= cfg.release_ticks || clicks >= cfg.max_clicks)
            {
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
