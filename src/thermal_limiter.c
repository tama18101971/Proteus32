/**
 * @file    thermal_limiter.c
 * @brief   Implementation of the zero-multiplication Thermal Power Limiter
 *          (see thermal_limiter.h for the full architectural rationale).
 *
 * @details
 *  Every stage below uses only:
 *    - 16-bit integer add/sub,
 *    - bitwise shifts (`<<` / `>>`) instead of multiply/divide,
 *    - compare-and-clamp,
 *  so it compiles to a small, deterministic RV32EC instruction sequence on
 *  the CH32V003 - no software multiply/divide routine is ever linked in for
 *  the control math itself.
 *
 *  Per tick, exactly one of three mutually-exclusive zones is evaluated:
 *    Zone 1 "Safe"         : t_cur <  t_threshold           -> P=0, I leaks.
 *    Zone 2 "Anticipation" : t_threshold <= t_cur < t_set    -> P=0, I charges.
 *    Zone 3 "Overheat"     : t_cur >= t_set                  -> P active, I charges.
 *
 *  The raw Limiter is `max(P, I)` (Intermittent Proportional + Dominant
 *  Integrator arbitration) rather than `P + I` - see step 5 below.
 */

#include "thermal_limiter.h"

/* ------------------------------------------------------------------------ */
/* Internal helpers                                                          */
/* ------------------------------------------------------------------------ */

/**
 * @brief Zero-mult decimation gate: fires (returns non-zero) only once
 *        every `2^shift` ticks - a free-running counter plus a single
 *        bitmask `AND`, no multiply/divide. `shift==0` fires every tick.
 *
 *        Takes a pointer to the CALLER-CHOSEN counter field rather than
 *        the whole state, so two independent gates with two independent
 *        shifts never share one counter:
 *          - charge_integral() gates on state->ki_tick_counter /
 *            config->ki_decimation_shift.
 *          - The Zone 2 overheat-latch leak gates on
 *            state->leak_tick_counter / config->overheat_leak_decimation_shift
 *            (see THERMAL_FLAG_OVERHEAT_LATCH / Zone 2 below).
 *        These MUST stay separate counters: if both gates incremented the
 *        SAME counter, the mask test would still be correct on AVERAGE
 *        over a long run in one zone, but the very FIRST fire right after
 *        a zone transition (e.g. Zone 3 charging -> Zone 2 latched leak)
 *        would land wherever the OTHER gate's calls happened to leave the
 *        counter's phase - anywhere from 1 tick to a full 2^shift ticks
 *        later, unpredictably, instead of a bounded, zone-relative delay.
 *        (This module used to share one counter for exactly that reason
 *        and it was wrong - see ::ThermalLimiterState_t::leak_tick_counter.)
 *
 * @param counter  Pointer to the specific free-running counter to use.
 * @param shift    Decimation shift - gate fires once every 2^shift ticks.
 * @return         Non-zero on ticks where the gated action should run.
 */
static inline uint8_t decimation_gate(uint8_t *counter, uint8_t shift)
{
    (*counter)++;
    uint8_t decim_mask = (uint8_t)((1u << shift) - 1u);
    return (*counter & decim_mask) == 0u;
}

/**
 * @brief Shift-charge the integrator by `delta << ki_left_shift`, gated by
 *        decimation_gate() and the Clamping Integral Lock, with
 *        overflow-safe wide arithmetic.
 *
 * @details
 *  RV32EC still carries a full 32-bit register file (RV32E only shrinks the
 *  register *count*, not their *width*) - so widening to int32_t here for
 *  the shift/add/clamp costs nothing extra in registers or cycles versus
 *  doing it in int16_t; it just avoids silently overflowing int16_t (UB)
 *  for aggressively configured `ki_left_shift` values. Nothing 32-bit is
 *  ever stored back into ::ThermalLimiterState_t - only int16_t crosses
 *  that boundary, preserving the module's RAM-footprint guarantee.
 *
 * @param state     Controller state (for the decimation tick counter).
 * @param config    Controller configuration (ki_left_shift/ki_decimation_shift).
 * @param integral  Current integrator value.
 * @param delta     Un-shifted charge basis: `t_cur - t_threshold` in Zone 2,
 *                  `t_cur - t_set` (i.e. `error`) in Zone 3 - both decay to
 *                  zero once t_cur settles back at the respective boundary,
 *                  so the integrator stops climbing once the temperature
 *                  plateaus instead of ratcheting up indefinitely.
 * @param p_term    This tick's Proportional term (Clamping Integral Lock test).
 * @param user_pwm  Current user brightness request (Clamping Integral Lock
 *                  test, and the upper clamp of the returned integral).
 * @return          Updated integral, already clamped to [0, user_pwm].
 */
static inline int16_t charge_integral(ThermalLimiterState_t *state,
                                       const ThermalLimiterConfig_t *config,
                                       int16_t integral,
                                       int16_t delta,
                                       int16_t p_term,
                                       uint16_t user_pwm)
{
    /* Clamping Integral Lock: P has already saturated the output at or
     * beyond user_pwm - stop charging, it can't add anything the
     * arbitration step downstream would actually use. */
    if (p_term >= (int16_t)user_pwm) {
        return integral;
    }

    if (!decimation_gate(&state->ki_tick_counter, config->ki_decimation_shift)) {
        return integral;
    }

    /* Overflow-safe shift + accumulate + clamp, all in one widened
     * expression (see @details above). */
    int32_t wide = (int32_t)integral + ((int32_t)delta << config->ki_left_shift);
    if (wide > (int32_t)user_pwm) {
        wide = (int32_t)user_pwm;
    } else if (wide < 0) {
        wide = 0;
    }
    return (int16_t)wide;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

void ThermalLimiter_Init(ThermalLimiterState_t *state)
{
    state->filtered_temp   = 0;
    state->integral         = 0;
    state->prev_limiter     = 0;
    state->flags            = 0u;
    state->ki_tick_counter  = 0u;
    state->leak_tick_counter = 0u;
}

uint16_t ThermalLimiter_Update(const ThermalLimiterConfig_t *config,
                                ThermalLimiterState_t *state,
                                uint16_t t_raw,
                                uint16_t t_set,
                                uint16_t user_pwm)
{
    /* --------------------------------------------------------------------
     * a) Failsafe: t_raw pinned at either ADC rail (0 or >=1023) can only
     *    happen if the NTC divider wire has broken open or shorted - a
     *    healthy divider never produces a rail-to-rail code. Bypass the
     *    entire loop (do NOT touch filtered_temp/integral with a garbage
     *    reading) and report the fault on the debug UART. This tick's
     *    Limiter is forced to user_pwm (therefore the applied PWM is 0)
     *    regardless, but
     *    `prev_limiter` itself is deliberately left untouched: forcing it
     *    to user_pwm here would make the slew filter treat every glitch -
     *    even a single noisy ADC sample - as if the LED had just been
     *    fully shut down, imposing a slow max_recovery_rate-limited ramp
     *    back to full brightness once the reading recovers, even though
     *    nothing was thermally wrong. Leaving it alone lets a transient,
     *    single-tick glitch resolve with zero extra recovery lag next
     *    tick, while a genuine, sustained fault still forces PWM=0 on
     *    every single affected tick (safety is never weakened - only the
     *    post-recovery smoothing behavior improves).
     * ------------------------------------------------------------------ */
    if (t_raw == THERMAL_ADC_MIN || t_raw >= THERMAL_ADC_MAX) {
        state->flags = THERMAL_FLAG_ERROR;
        /* This API returns the limiter amount, not the applied PWM. */
        return user_pwm;
    }

    /* Seed the filter on the very first call so the EMA does not "ramp up"
     * from 0 -> t_raw, which would otherwise look like a huge fake
     * temperature spike to the control loop. */
    if (!(state->flags & THERMAL_FLAG_INITIALIZED)) {
        state->filtered_temp = (int16_t)t_raw;
        state->flags        |= THERMAL_FLAG_INITIALIZED;
    }

    /* --------------------------------------------------------------------
     * b) Raw Low-Pass Filtering - EMA computed directly in the raw 10-bit
     *    ADC domain, no separate scaled "temperature" variable:
     *      filtered_temp += (t_raw - filtered_temp) >> filter_shift
     *    The ">> filter_shift" is the *only* place the division-by-2^n is
     *    replaced with a shift; t_cur below is simply filtered_temp itself.
     * ------------------------------------------------------------------ */
    state->filtered_temp = (int16_t)(state->filtered_temp +
                            (((int16_t)t_raw - state->filtered_temp) >> config->filter_shift));

    int16_t t_cur = state->filtered_temp;

    /* --------------------------------------------------------------------
     * c) Two-Zone Intermittent Error & Integration Logic.
     *
     *    t_threshold = t_set - t_anticipate_offset, clamped to >= 0 so a
     *    mis-configured offset larger than t_set cannot underflow into a
     *    huge positive value when read back as unsigned elsewhere - kept
     *    signed int16_t here since t_set/offset are both well within the
     *    10-bit ADC range (<=1023), no overflow risk.
     * ------------------------------------------------------------------ */
    int16_t t_threshold = (int16_t)t_set - (int16_t)config->t_anticipate_offset;
    if (t_threshold < 0) {
        t_threshold = 0;
    }

    int16_t p_term   = 0;             /* Proportional term - only Zone 3 sets this. */
    int16_t integral = state->integral;

    if (t_cur < t_threshold) {
        /* ------------------------------------------------------------
         * Zone 1 - Safe: no proportional or integral charge. Any integral
         * left over from a previous limiting event is bled off with a
         * two-tier decay:
         *   - "Very cold"  (t_cur below the fast-leak line): fast leak,
         *     ~25% of itself every tick (`integral -= integral >> 2`) for
         *     quick recovery.
         *   - "Near threshold": gentle leak, subtract 1/32nd of itself
         *     each tick (`integral -= integral >> 5`).
         *
         * Two fixes vs. a naive two-tier decay:
         *
         *  1) Hysteresis latch (THERMAL_FLAG_FAST_LEAK): the switch
         *     between fast/slow decay is a Schmitt trigger, not a bare
         *     comparison - entering fast-leak requires dropping below
         *     `fast_leak_enter`, but *leaving* fast-leak requires climbing
         *     back past `fast_leak_enter + THERMAL_FAST_LEAK_HYSTERESIS`.
         *     Without this, t_cur sitting right on the boundary (EMA/ADC
         *     noise) would flip the decay rate every tick.
         *
         *  2) Guaranteed-zero floor: `integral >> THERMAL_SLOW_LEAK_SHIFT`
         *     (or `>> THERMAL_FAST_LEAK_SHIFT`) truncates to 0 for any
         *     |integral| smaller than the shift denominator, so a bare
         *     `integral -= integral >> shift` would stall forever at some
         *     small residual instead of actually reaching 0. Forcing a
         *     minimum decay step of 1 whenever the shift alone would
         *     produce 0 guarantees the accumulator empties completely in
         *     a bounded number of ticks.
         * ------------------------------------------------------------ */

        /* Truly safe again - disarm the overheat latch so the next
         * excursion's Zone 2 anticipation charging works normally (see
         * THERMAL_FLAG_OVERHEAT_LATCH doc in thermal_limiter.h). Also
         * reset leak_tick_counter so the NEXT excursion's first leak fire
         * (after it re-enters latched Zone 2) has a bounded, predictable
         * delay of at most 2^overheat_leak_decimation_shift ticks, instead
         * of inheriting whatever phase this excursion happened to leave
         * behind. Also clear Zone 3 hysteresis latch: well below
         * t_threshold there is no question about which zone we're in. */
        state->flags &= (uint8_t)~(THERMAL_FLAG_OVERHEAT_LATCH | THERMAL_FLAG_IN_ZONE3);
        state->leak_tick_counter = 0u;

        int16_t fast_leak_enter = (int16_t)(t_threshold - THERMAL_FAST_LEAK_OFFSET);
        int16_t fast_leak_exit  = (int16_t)(fast_leak_enter + THERMAL_FAST_LEAK_HYSTERESIS);

        if (state->flags & THERMAL_FLAG_FAST_LEAK) {
            if (t_cur >= fast_leak_exit) {
                state->flags &= (uint8_t)~THERMAL_FLAG_FAST_LEAK;
            }
        } else {
            if (t_cur < fast_leak_enter) {
                state->flags |= THERMAL_FLAG_FAST_LEAK;
            }
        }

        if (state->flags & THERMAL_FLAG_FAST_LEAK) {
            int16_t decay = integral >> THERMAL_FAST_LEAK_SHIFT; /* fast leak (~25%/tick) */
            if (decay == 0 && integral > 0) {
                decay = 1;                                     /* guarantee reaching 0 */
            }
            integral -= decay;
        } else {
            int16_t decay = integral >> THERMAL_SLOW_LEAK_SHIFT; /* slow leak (/32) */
            if (decay == 0 && integral > 0) {
                decay = 1;                                     /* guarantee reaching 0 */
            }
            integral -= decay;
        }

    } else if (t_cur < (int16_t)t_set &&
               !(state->flags & THERMAL_FLAG_IN_ZONE3)) {
        /* ------------------------------------------------------------
         * Zone 2 - Anticipation: gentle early limiting. P stays 0 to avoid
         * jitter right around the threshold; only the dominant Integrator
         * charges, shift-scaled (no Ki multiplication) via the shared,
         * overflow-safe, decimation-gated helper (see charge_integral()):
         *   error = t_cur - t_threshold          (0 .. t_anticipate_offset)
         *
         * The THERMAL_FLAG_IN_ZONE3 guard: if we just left Zone 3 on the
         * previous tick but t_cur hasn't yet fallen below
         * t_set - THERMAL_ZONE3_HYSTERESIS, this branch is skipped and
         * the controller stays in Zone 3 instead. This prevents rapid
         * toggling between Zone 2 and Zone 3 when EMA noise bounces
         * t_cur around t_set (e.g. 699,700,699,700...), which would
         * otherwise cause P to flicker on/off every tick and
         * OVERHEAT_LATCH to be constantly re-armed.
         *
         * Overheat-latch gate: this charge basis only decays to zero at
         * t_threshold, which can be tens of ADC codes below t_set - if
         * t_cur has ALREADY reached Zone 3 once this excursion (a real
         * overheat happened and P/I already corrected it), continuing to
         * charge while merely retreating back down through this whole
         * band re-inflates the integrator for a threat that has already
         * passed, well past what's needed to hold at t_set. While the
         * latch is set (see THERMAL_FLAG_OVERHEAT_LATCH), skip charging
         * and instead gently leak the integrator so brightness recovers
         * smoothly as t_cur keeps falling, rather than freezing solid
         * until t_cur happens to cross all the way down to t_threshold.
         * The latch only re-arms Zone 2's charging once t_cur genuinely
         * drops below t_threshold (Zone 1) and clears it.
         *
         * Three leak designs were tried and rejected before this one:
         *   - Plain hold (no leak at all): brightness stayed frozen for
         *     as long as t_cur takes to fall the full t_anticipate_offset
         *     back to t_threshold - too slow to feel like it's recovering.
         *   - THERMAL_SLOW_LEAK_SHIFT every tick (Zone 1's rate): a
         *     percentage-of-current-value decay is large when integral is
         *     large, draining a large integral in a handful of seconds
         *     off a single noisy EMA/ADC downward tick - snapping
         *     brightness back to 100% on noise and risking an immediate
         *     re-overheat.
         *   - THERMAL_OVERHEAT_LATCH_LEAK_SHIFT alone, applied every tick,
         *     no further decimation: for the whole realistic integral
         *     range (0..511) that shift's `integral >> shift` already
         *     floors to 0, so the "guarantee reaching 0" rule forces a
         *     constant ~1 unit/tick (~30/sec) regardless of how large the
         *     shift is made - raising the shift further cannot slow this
         *     down any more. A brief few-second noise dip right at t_set
         *     still fully drained a large integral (e.g. 200+) before
         *     t_cur even resumed rising, forcing a full recharge from
         *     scratch on the very next excursion.
         * The fix: gate the (still floor-guaranteed ~1 unit) leak by
         * config->overheat_leak_decimation_shift, a cadence completely
         * independent of the charge's ki_decimation_shift - this is the
         * only remaining knob able to slow the leak below ~30/sec, since
         * the per-fire amount is already pinned at its floor.
         *
         * Uses its OWN counter (state->leak_tick_counter), never
         * state->ki_tick_counter - see decimation_gate() @details for why
         * sharing one counter between two differently-shifted gates gives
         * the leak's first fire after a zone transition an unpredictable
         * phase instead of a bounded one.
         * ------------------------------------------------------------ */
        if (state->flags & THERMAL_FLAG_OVERHEAT_LATCH) {
            if (decimation_gate(&state->leak_tick_counter, config->overheat_leak_decimation_shift)) {
                int16_t decay = integral >> THERMAL_OVERHEAT_LATCH_LEAK_SHIFT;
                if (decay == 0 && integral > 0) {
                    decay = 1; /* guarantee reaching 0, same rationale as Zone 1 */
                }
                integral -= decay;
            }
        } else {
            int16_t error = t_cur - t_threshold;
            integral = charge_integral(state, config, integral, error, p_term, user_pwm);
        }

    } else {
        /* ------------------------------------------------------------
         * Zone 3 - Overheat: full aggressive Proportional correction plus
         * continued Integrator charging.
         *   error  = t_cur - t_set                         (>= 0)
         *   P      = error << kp_left_shift                (fast reaction,
         *            overflow-safe via widened intermediate - see below)
         *   integral charges from `error` (t_cur - t_set), NOT from
         *     (t_cur - t_threshold): charging must decay to zero once
         *     t_cur settles back at t_set, otherwise the integrator keeps
         *     ratcheting up by a constant ~t_anticipate_offset every
         *     decimation window even after the temperature has plateaued
         *     exactly at the set-point, eventually saturating for no
         *     reason and causing a large limit-cycle swing. This does
         *     introduce a small downward step in charge rate right at the
         *     Zone2->Zone3 boundary (from ~t_anticipate_offset down to 0)
         *     instead of a continuous ramp, but that step is harmless:
         *     Zone 2 has already charged the integrator while approaching
         *     t_set, so Zone 3 only needs to add MORE charge if t_cur
         *     actually overshoots past t_set - not just for sitting at it.
         *
         *     Zone 3 hysteresis: set THERMAL_FLAG_IN_ZONE3 here, cleared
         *     only when t_cur falls below t_set - THERMAL_ZONE3_HYSTERESIS
         *     (Zone 1 already clears it unconditionally at t_threshold).
         *     While set, the Zone 2 branch is bypassed entirely, which
         *     prevents the P term, integrator charging law, and
         *     OVERHEAT_LATCH from toggling every tick when EMA/ADC noise
         *     bounces t_cur right around t_set.
         * ------------------------------------------------------------ */

        /* A real overheat is happening right now - arm the latch so that
         * once t_cur retreats back into Zone 2, that zone's anticipation
         * charging stays suppressed (see THERMAL_FLAG_OVERHEAT_LATCH doc
         * and the Zone 2 gate above). */
        state->flags |= THERMAL_FLAG_OVERHEAT_LATCH | THERMAL_FLAG_IN_ZONE3;

        int16_t error = t_cur - (int16_t)t_set;

        /* Overflow-safe P: widen to int32_t for the shift (free on RV32EC's
         * 32-bit register file, see charge_integral() @details), then clamp
         * to THERMAL_PWM_MAX before narrowing - any P at or above that is
         * indistinguishable downstream from a smaller-but-still-saturating
         * P (both mean "fully limited"), so clamping loses no information
         * while making the narrowing to int16_t always well-defined. */
        int32_t p_wide = (int32_t)error << config->kp_left_shift;
        if (p_wide > (int32_t)THERMAL_PWM_MAX) {
            p_wide = (int32_t)THERMAL_PWM_MAX;
        }
        p_term = (int16_t)p_wide;

        integral = charge_integral(state, config, integral, error, p_term, user_pwm);

        /* Hysteresis exit: drop the Zone 3 latch once t_cur genuinely
         * falls below t_set - THERMAL_ZONE3_HYSTERESIS, not just one
         * noisy code below t_set. This prevents the Zone 2 / Zone 3
         * toggle that would otherwise happen when EMA noise bounces
         * t_cur around t_set (see THERMAL_FLAG_IN_ZONE3 doc). */
        if (t_cur < (int16_t)((int16_t)t_set - (int16_t)THERMAL_ZONE3_HYSTERESIS)) {
            state->flags &= (uint8_t)~THERMAL_FLAG_IN_ZONE3;
        }
    }

    /* --------------------------------------------------------------------
     * d) Dynamic 16-bit Anti-Windup: keep the integrator directly scaled
     *    within [0, user_pwm] - a simple compare-and-clamp, no division.
     * ------------------------------------------------------------------ */
    if (integral > (int16_t)user_pwm) {
        integral = (int16_t)user_pwm;
    }
    if (integral < 0) {
        integral = 0;
    }
    state->integral = integral;

    /* --------------------------------------------------------------------
     * Intermittent Core Arbitration: raw_limiter = max(P, I) instead of
     * P + I - the Integrator is the dominant baseline, P is only an
     * intermittent spike-correction that briefly outranks it in Zone 3.
     * Clamp to the valid [0, user_pwm] Limiter domain before the slew
     * filter runs (P is unclamped above and can exceed user_pwm).
     * ------------------------------------------------------------------ */
    int16_t raw_limiter = (p_term > integral) ? p_term : integral;
    if (raw_limiter < 0) {
        raw_limiter = 0;
    }
    if (raw_limiter > (int16_t)user_pwm) {
        raw_limiter = (int16_t)user_pwm;
    }

    /* --------------------------------------------------------------------
     * e) Unified Slew Rate Filter - single signed delta + single-pass
     *    clamp instead of separate increasing/decreasing branch trees.
     *      delta > max_drop_rate      -> capped growth  (heating up)
     *      delta < -max_recovery_rate -> capped decay    (cooling down)
     *      else                       -> pass target through unchanged
     *    max_recovery_rate should be configured 2-3x smaller than
     *    max_drop_rate: cut brightness fast, restore it slowly.
     * ------------------------------------------------------------------ */
    int16_t last_limiter = state->prev_limiter;
    int16_t delta         = raw_limiter - last_limiter;
    int16_t next_limiter;

    if (delta > (int16_t)config->max_drop_rate) {
        next_limiter = (int16_t)(last_limiter + (int16_t)config->max_drop_rate);
    } else if (delta < -(int16_t)config->max_recovery_rate) {
        next_limiter = (int16_t)(last_limiter - (int16_t)config->max_recovery_rate);
    } else {
        next_limiter = raw_limiter;
    }

    /* Cheap safety net: the slew step above can in principle push the
     * value slightly outside [0, user_pwm] (e.g. right after user_pwm is
     * lowered externally). Still just compares - no division. */
    if (next_limiter < 0) {
        next_limiter = 0;
    }
    if (next_limiter > (int16_t)user_pwm) {
        next_limiter = (int16_t)user_pwm;
    }

    /* Hard brightness floor: max_limiter caps how far the Limiter can
     * climb, guaranteeing pwm_out = user_pwm - limiter >= user_pwm - max_limiter.
     * Applied AFTER slew filter so the rate limiter cannot mask this cap. */
    if (next_limiter > (int16_t)config->max_limiter) {
        next_limiter = (int16_t)config->max_limiter;
    }

    state->prev_limiter = next_limiter;

    /* --------------------------------------------------------------------
     * Final limiter amount. The caller applies it as:
     *     pwm_out = user_pwm - limiter
     * Safe by construction: 0 <= next_limiter <= user_pwm.
     * ------------------------------------------------------------------ */
    return next_limiter;
}
