/**
 * @file    thermal_limiter.h
 * @brief   Ultra-optimized, zero-multiplication Thermal Power Limiter for a
 *          high-power LED, targeting the WCH CH32V003 (RV32EC, no hardware
 *          divider, costly software multiply, 16KB Flash / 2KB RAM).
 *
 * @details
 *  Architecture: "Intermittent Proportional + Dominant Integrator".
 *  ------------------------------------------------------------------------
 *  Classic academic PI controllers sum `Limiter = P + I`. On a core with no
 *  hardware divider and expensive multiplication this is both wasteful (two
 *  scaled terms have to be computed every tick even when only one matters)
 *  and hard to tune without real multiplies for the gains.
 *
 *  Instead this module treats:
 *    - the **Integrator** as the slow, dominant thermal baseline model
 *      (it is what actually holds the LED at the set-point over time), and
 *    - the **Proportional** term as a fast, intermittent correction that
 *      only exists at all once the system is at/above `t_set` (Zone 3),
 *      used to catch rapid thermal spikes the slow integrator can't react
 *      to in time.
 *
 *  The two terms are never added. Every tick the raw limiter is simply
 *  `max(P, I)` (see ::ThermalLimiter_Update / thermal_limiter.c step 5) -
 *  whichever term currently demands more brightness reduction wins.
 *
 *  Zero-Multiplication / Zero-Division Constraint:
 *  ------------------------------------------------------------------------
 *  There is NOT a single `*` or `/` anywhere in the hot loop. All "gains"
 *  (`kp_left_shift`, `ki_left_shift`) are bit counts consumed by `<<`, and
 *  all decays/filters use `>>`. This compiles down to nothing but
 *  add/sub/shift/compare RV32EC instructions - no call to a software
 *  multiply or divide routine is ever emitted for the control math itself.
 *
 *  Three-Zone Error Model (relative to `t_threshold = t_set - t_anticipate_offset`):
 *  ------------------------------------------------------------------------
 *    Zone 1 "Safe"        (t_cur <  t_threshold)            -> P=0, I decays.
 *    Zone 2 "Anticipation"(t_threshold <= t_cur < t_set)     -> P=0, I charges
 *                                                                (or gently
 *                                                                decays if the
 *                                                                overheat
 *                                                                latch is set
 *                                                                - see
 *                                                                THERMAL_FLAG_OVERHEAT_LATCH).
 *    Zone 3 "Overheat"    (t_cur >= t_set)                   -> P active, I charges,
 *                                                                latch set.
 *
 *  Failsafe:
 *  ------------------------------------------------------------------------
 *  A `t_raw` reading of exactly 0 or >= 1023 (10-bit rail-to-rail) can only
 *  happen if the NTC divider has failed open or shorted. In that case the
 *  entire control loop is bypassed for that tick: the Limiter is forced to
 *  `user_pwm` (i.e. output PWM = 0, LED fully OFF) and `"E_THERMAL_RUNAWAY\n"`
 *  is emitted on the debug UART.
 *
 *  Register/type footprint:
 *  ------------------------------------------------------------------------
 *  The persistent state (::ThermalLimiterState_t) is built entirely out of
 *  `int16_t` / `uint8_t` fields - no `int32_t` is used anywhere in the loop
 *  or state, since RV32EC still has to move/mask 32-bit values through the
 *  same 32-bit register file with extra sign-extension overhead when they
 *  are stored back into narrower memory; keeping everything 16-bit-wide in
 *  memory (and letting the compiler widen transiently in registers only
 *  where the intermediate `<<`/`>>` truly needs it) keeps both the RAM
 *  footprint and the generated instruction count minimal.
 */

#ifndef THERMAL_LIMITER_H
#define THERMAL_LIMITER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Compile-time constants                                                   */
/* ------------------------------------------------------------------------ */

/** 10-bit ADC domain limits for `t_raw` - values at/beyond these rails are
 *  physically impossible for a healthy NTC divider and indicate a broken or
 *  shorted sensor wire (see ::ThermalLimiter_Update failsafe check). */
#define THERMAL_ADC_MIN            0u
#define THERMAL_ADC_MAX            1023u

/** 9-bit user PWM / Limiter domain (0..512 inclusive, per spec). */
#define THERMAL_PWM_MAX            512u

/** Zone 1 "very cold" fast-leak offset (ADC counts below `t_threshold`
 *  at which the integrator is halved instead of slow-leaked), per spec. */
#define THERMAL_FAST_LEAK_OFFSET   30

/** Zone 1 "near threshold" slow-leak shift: `integral -= (integral >> 5)`. */
#define THERMAL_SLOW_LEAK_SHIFT    5u

/**
 * Zone 2 overheat-latch leak shift: `integral -= (integral >> 8)`, fired on
 * the cadence gated by ThermalLimiterConfig_t::overheat_leak_decimation_shift
 * (NOT every tick, unlike THERMAL_SLOW_LEAK_SHIFT). Deliberately much
 * gentler than Zone 1's slow-leak: for the whole realistic 0..511 integral
 * range this shift alone truncates to a fixed ~1 unit *per fire* (the
 * "guarantee reaching 0" floor dominates), so raising this constant further
 * cannot slow the leak down any more - overheat_leak_decimation_shift is
 * the actual speed knob, controlling how often that ~1-unit decrement
 * fires. See THERMAL_FLAG_OVERHEAT_LATCH / thermal_limiter.c Zone 2 for
 * the full history of leak designs tried and rejected before this one.
 */
#define THERMAL_OVERHEAT_LATCH_LEAK_SHIFT 8u

/**
 * Hysteresis band (ADC counts) between the fast-leak *enter* line
 * (`t_threshold - THERMAL_FAST_LEAK_OFFSET`) and the *exit* line the
 * filtered temperature must climb back past before the decay switches
 * back from fast (/2) to slow (/32). Without this band, `t_cur` sitting
 * right on the boundary (residual EMA/ADC noise) would flip the decay
 * rate every tick - see ::ThermalLimiterState_t::flags /
 * THERMAL_FLAG_FAST_LEAK and thermal_limiter.c Zone 1 for the latch logic.
 */
#define THERMAL_FAST_LEAK_HYSTERESIS 8

/** Bits packed into ::ThermalLimiterState_t::flags. Kept as a single
 *  uint8_t bitfield (instead of two separate bool fields) so the extra
 *  fast-leak latch below costs zero additional RAM. */
#define THERMAL_FLAG_INITIALIZED  0x01u /**< EMA filter has been seeded once. */
#define THERMAL_FLAG_FAST_LEAK    0x02u /**< Zone 1 fast-leak hysteresis latch
                                              (see THERMAL_FAST_LEAK_HYSTERESIS). */
#define THERMAL_FLAG_OVERHEAT_LATCH 0x04u /**< Set the moment t_cur first
                                                reaches Zone 3 (t_cur >= t_set)
                                                during the current excursion
                                                above t_threshold. While set,
                                                Zone 2 stops charging and
                                                instead gently leaks the
                                                integrator (same slow-leak
                                                formula as Zone 1): a real
                                                overheat has already happened
                                                and been corrected, so
                                                continuing to charge while
                                                merely retreating back down
                                                through the anticipation band
                                                would just re-inflate the
                                                integrator for a threat that
                                                already passed - but freezing
                                                it solid instead would stall
                                                brightness recovery until
                                                t_cur happened to cross all
                                                the way down to t_threshold.
                                                Cleared only once t_cur
                                                actually drops below
                                                t_threshold (Zone 1), arming
                                                Zone 2's charging again for
                                                the next excursion. */

#define THERMAL_FLAG_ERROR        0x08u /**Аппаратная ошибка. */

/* ------------------------------------------------------------------------ */
/* Configuration (read-only, typically `const` / flash-resident)            */
/* ------------------------------------------------------------------------ */

/**
 * @brief Tunable, shift-based parameters of the thermal limiter.
 *
 * Every gain here is a bit-shift count, not a multiplicative coefficient -
 * there is no Q-format scale factor to divide back out. Intended to live in
 * flash (`static const`) since it never changes at runtime.
 *
 * Effective integral gain & thermal-inertia note:
 * ------------------------------------------------------------------------
 * `ki_left_shift` alone can only ever *amplify* (minimum factor is 1x at
 * shift=0) - there is no symmetric right-shift to attenuate it, since the
 * spec forbids anything but left-shift gains. For a plant with large
 * thermal mass/inertia the filtered error can sit at a small, roughly
 * constant value for hundreds of ticks (many seconds) while dwelling in
 * Zone 2/3; charging the integrator by at least 1x that error *every
 * single tick* at 30Hz saturates it against user_pwm in a couple of
 * seconds - long before the real thermal event would warrant a full
 * shutdown. `ki_decimation_shift` fixes this without ever introducing a
 * multiply or divide: the integral is only charged once every
 * `2^ki_decimation_shift` ticks (gated by a free-running counter and a
 * bitmask `AND`), giving an effective, still zero-mult gain of
 * `2^(ki_left_shift - ki_decimation_shift)` - i.e. a genuine fractional
 * Ki. Set to 0 (default) to keep the original "charge every tick"
 * behavior; raise it (e.g. 5-7) for slow, high-thermal-mass objects so
 * the integrator ramps over the same many-second timescale the real
 * plant does, instead of slamming into anti-windup almost immediately.
 *
 * `overheat_leak_decimation_shift` is the same zero-mult decimation
 * technique applied to the OTHER direction: how fast the integrator is
 * allowed to forget a past overheat event once retreating back through
 * Zone 2 (see THERMAL_FLAG_OVERHEAT_LATCH). It is independent of
 * `ki_decimation_shift` - charge and leak cadence do not have to match.
 */
typedef struct {
    uint8_t  kp_left_shift;        /**< Proportional gain, Zone 3 only:
                                         P = (t_cur - t_set) << kp_left_shift. */
    uint8_t  ki_left_shift;        /**< Integral gain, Zones 2 & 3:
                                         integral += (error << ki_left_shift). */
    uint8_t  ki_decimation_shift;  /**< Zero-mult "fractional Ki" gate: only charge
                                         the integrator once every 2^ki_decimation_shift
                                         ticks. 0 = charge every tick (original
                                         behavior). See note above - use larger
                                         values (5-7) for high-thermal-inertia
                                         objects to avoid near-instant integrator
                                         saturation. Recommended range: 0..7. */
    uint16_t t_anticipate_offset;  /**< ADC counts below t_set where Zone 2
                                         ("anticipation") begins:
                                         t_threshold = t_set - t_anticipate_offset. */
    uint8_t  filter_shift;         /**< EMA filter shift, applied directly in the
                                         raw ADC domain:
                                         filtered_temp += (t_raw - filtered_temp) >> filter_shift. */
    uint16_t max_drop_rate;        /**< Max Limiter increase per tick (brightness
                                         cut speed while heating up). */
    uint16_t max_recovery_rate;    /**< Max Limiter decrease per tick (brightness
                                         restore speed while cooling down). Should
                                         be 2-3x smaller than max_drop_rate to avoid
                                         visible flicker/surges. */
    uint8_t  max_limiter;          /**< Hard ceiling on the Limiter output (0..255).
                                         When reached, brightness cannot be cut any
                                         further regardless of P or I terms.
                                         Ensures a minimum brightness floor:
                                         pwm_out >= user_pwm - max_limiter. */
    uint8_t  overheat_leak_decimation_shift; /**< Zone 2 overheat-latch leak gate
                                         (see THERMAL_FLAG_OVERHEAT_LATCH): the
                                         integrator loses a fixed ~1 unit only
                                         once every 2^overheat_leak_decimation_shift
                                         ticks, instead of every tick. Needed
                                         because THERMAL_OVERHEAT_LATCH_LEAK_SHIFT's
                                         per-tick amount is already floored to a
                                         minimum of 1 for the whole realistic
                                         integral range (0..511) - increasing that
                                         shift further cannot slow the leak down
                                         any more, since the floor already
                                         dominates. This decimation is the actual
                                         speed knob: too fast (0-1) and a brief
                                         few-second noise dip right at t_set fully
                                         drains a large integral before the
                                         temperature even resumes rising, forcing
                                         a full recharge from scratch; too slow
                                         and genuine cooling won't visibly recover
                                         brightness for a long time. Recommended
                                         range: 1..4. */
} ThermalLimiterConfig_t;

/* ------------------------------------------------------------------------ */
/* Runtime state (RAM-resident, minimal footprint)                          */
/* ------------------------------------------------------------------------ */

/**
 * @brief Persistent state of a single thermal limiter instance.
 *
 * Entirely 16-bit-or-narrower fields - no int32_t. Total footprint is 8
 * bytes (no extra alignment padding vs. the previous 7-byte/8-byte-padded
 * layout - ki_tick_counter exactly fills what used to be a padding byte),
 * leaving ample headroom in the 2KB RAM budget for the rest of the
 * application.
 */
typedef struct {
    int16_t  filtered_temp;  /**< EMA-filtered raw ADC value (same domain/scale
                                   as t_raw, i.e. 0..1023 nominally). This IS
                                   `t_cur` - no separate scaled temperature
                                   variable exists. */
    int16_t  integral;       /**< Dominant integrator accumulator, directly
                                   scaled in Limiter units and clamped to
                                   [0, user_pwm] every tick (see step 4d). */
    int16_t  prev_limiter;   /**< Previous tick's post-slew-limit Limiter
                                   value (0..512), used by the slew filter. */
    uint8_t  flags;           /**< Bitfield, see THERMAL_FLAG_INITIALIZED /
                                   THERMAL_FLAG_FAST_LEAK above. Packed into a
                                   single byte (instead of two bool fields) so
                                   the Zone 1 hysteresis latch is free in terms
                                   of RAM footprint. */
    uint8_t  ki_tick_counter;/**< Free-running tick counter used only to gate
                                   integral charging per config->ki_decimation_shift
                                   (zero-mult fractional-Ki substitute, see
                                   ThermalLimiterConfig_t doc above). Wraps
                                   naturally at 256, harmless since the gate
                                   mask is always a power-of-2 <= 128. */
} ThermalLimiterState_t;

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

/**
 * @brief Reset a limiter instance to its power-on-safe state.
 *
 * Must be called once before the first ThermalLimiter_Update() call.
 *
 * @param state  State instance to initialize.
 */
void ThermalLimiter_Init(ThermalLimiterState_t *state);

/**
 * @brief Run one iteration of the thermal power limiter control loop.
 *
 * Non-blocking, deterministic execution time - a fixed handful of integer
 * add/sub/shift/compare operations, no multiply, no divide. Safe to call
 * from a timer ISR or a ~30Hz task.
 *
 * @param config    Controller configuration/tuning (typically `static const`).
 * @param state     Persistent controller state (RAM).
 * @param t_raw     Raw 10-bit ADC reading from the NTC divider (0..1023).
 *                  Higher value == higher temperature (NTC in upper arm).
 *                  A value of exactly 0 or >= 1023 triggers the failsafe
 *                  (sensor open/short) and immediately shuts down the LED.
 * @param t_set     10-bit ADC target set-point (0..1023).
 * @param user_pwm  9-bit user-requested brightness (0..512). The controller
 *                  output can only reduce this value, never exceed it.
 *
 * @return          Final PWM duty value to apply to the LED driver
 *                   (0..user_pwm).
 */
uint16_t ThermalLimiter_Update(const ThermalLimiterConfig_t *config,
                                ThermalLimiterState_t *state,
                                uint16_t t_raw,
                                uint16_t t_set,
                                uint16_t user_pwm);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_LIMITER_H */
