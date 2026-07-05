# AGENTS.md — Guide for AI Assistants

## Project Overview

**Proteus003** is a flashlight firmware based on **CH32V003F4U6** (RISC-V RV32EC, 48 MHz, 16KB Flash, 2KB RAM).

### Key Features
- **LED Thermal Protection**: NTC thermistor via ADC → ThermalLimiter → PWM limiting
- **Sleep Mode**: MCU sleeps in STANDBY between cycles, wakes up on AWU or button
- **Direct Register Writes**: All peripheral registers are configured directly (without SDK helpers)
- **Energy Saving**: ADC, TIM2, and I2C are fully disabled when entering sleep
- **UART 460800 baud**: High-speed debugging, does not block ramping

### Framework
The project uses **WCH noneos-sdk** (WCH Standard Peripheral Library, no RTOS). Some HAL functions (`GPIO_Init`, `ADC_RegularChannelConfig`, etc.) are used in `main.c` for peripheral initialization, but the core algorithm (`thermal_limiter.c`) works without HAL.

---

## ⚠️ PROHIBITED: DO NOT USE STM32 INFORMATION

**CH32V003 is NOT STM32!** Their registers, peripherals, and behavior are DIFFERENT.

### Specific Differences:
| Parameter | STM32F1 | CH32V003 |
|----------|---------|----------|
| Core | ARM Cortex-M | RISC-V (QingKe V2A) |
| I2C Base Address | 0x40005400 | 0x40005400 (same, but registers are different!) |
| GPIO CFGLR | 4 bits/pin (CNF+MODE) | 4 bits/pin (similar, but SDK masks Mode via & 0x0F) |
| I2C CKCFGR | DUTY=bit 14 | DUTY=bit 14, but CCR formula is different |
| AWU Timer | No | Yes (LSI 128kHz, EXTI Line9) |
| USART | Multiple USART | Only USART1 |
| Flash | Large | 16KB (last 256 bytes reserved for data) |
| RAM | Large | 2KB |

### CRITICALLY IMPORTANT:
1. **Do not copy code from STM32 examples** — registers may have the same names but behave differently
2. **Do not use STM32 HAL/StdPeriph** — the project uses direct registers of CH32V003
3. **Do not assume STM32 behavior** — for example, CFGLR on CH32V003 masks Mode via `& 0x0F` before writing
4. **Check CH32V003 datasheet**, not STM32 when working with peripherals

---

## Build and Run

```powershell
pio run                 # Build
pio run -t clean        # Clean artifacts
pio run -t upload       # Load to board (WCH-Link required)
pio device monitor      # UART monitor (115200 8N1)
```

**Important**: The project is built ONLY through PlatformIO. Do not use cmake, make, or other build systems.

### Warning Check
After any changes to `thermal_limiter.c`/`.h` or `main.c`:
1. Temporarily add `-Wall -Wextra` to `build_flags` in `platformio.ini`
2. `pio run -t clean; pio run` — ensure there are no warnings in `src/main.c` or `src/thermal_limiter.c`
3. **Revert** `platformio.ini` to original state (do not commit `-Wall -Wextra`)

Warnings from framework files (`.pio/`, `framework-wch-noneos-sdk`) — ignore, it's not our code.

---

## Project Structure

```
include/
├── thermal_limiter.h   — Public API: ThermalLimiterConfig_t, ThermalLimiterState_t,
│                         ThermalLimiter_Init/Update. Extensive comments — read first.
├── limiters.h          — Wrapper: sys_init(), scan_limiters(), reset_limiters()
├── main.h              — Global declarations
├── config.h            — Debug settings (WL_FLASH_DEBUG, PRINT_ADC, EE_PRINTF)
│
├── awu.h               — Auto Wake-Up timer
├── timers.h            — TIM2 (1ms tick, timer intervals)
├── button_gpio.h       — Button (EXTI Line3 on PC3)
├── aux_leds.h          — AUX LED automaton (LED blinking)
├── ui.h                — User interface (brightness ramping)
├── soft_start.h        — Soft start
├── led_gpio.h          — LED GPIO control (PC4, PC6, PC7)
├── dac.h               — DAC7571 + I2C (brightness control)
├── i2c.h               — I2C driver (direct registers)
├── adc.h               — ADC (NTC + Vrefint)
├── wl_flash.h          — Flash data storage
├── array.h             — Array utilities
├── ch32v003_gpio.h     — GPIO utilities
└── mini_printf.h       — Mini printf implementation

src/
├── main.c              — Entry point, main loop, HAL integration
├── thermal_limiter.c   — Core algorithm for thermal protection (zero-multiplication)
├── limiters.c          — Wrapper: initialization, scan_limiters(), reset_limiters()
│
├── awu.c               — Auto Wake-Up timer (direct registers)
├── timers.c            — TIM2 (direct registers)
├── button_gpio.c       — Button (EXTI)
├── aux_leds.c          — AUX LED automaton
├── ui.c                — User interface
├── soft_start.c        — Soft start
├── led_gpio.c          — LED GPIO control
├── dac.c               — DAC7571 (direct registers)
├── i2c.c               — I2C driver (direct registers)
├── adc.c               — ADC (NTC + Vrefint)
├── wl_flash.c          — EEPROM emulation in Flash
├── array.c             — Array utilities
├── ob.c                — Option bytes
└── mini_printf.c       — Mini printf implementation

Project root:
├── platformio.ini      — Build configuration
├── TUNING.md           — Step-by-step parameter tuning procedure (Russian)
├── tz_new.md           — Current architecture specification (authoritative)
├── tzq.md              — Outdated specification (historical, DO NOT use)
└── AGENTS.md           — This file
```

---

## Thermal Protection Architecture (ThermalLimiter)

### Purpose
Limit LED power to protect against overheating. The controller reads the NTC thermistor via ADC, calculates how much to reduce the user-requested PWM brightness, and returns the final duty cycle value. **Does NOT control hardware PWM directly** — caller (`main.c`) integrates the result into their output.

### Architecture: "Intermittent Proportional + Dominant Integrator"
A classic PI controller sums `Limiter = P + I`. Instead:
- **Integrator** — slow, dominant thermal base model (holds LED at setpoint)
- **Proportional** — fast, periodic correction, active only in Zone 3 (overheat)
- Arbitration: `Limiter = max(P, I)` — whichever term requires more limiting wins

### Three Error Zones (relative to `t_threshold = t_set - t_anticipate_offset`):
| Zone | Entry Condition | Exit Condition | P | I |
|------|-----------------|----------------|---|---|
| 1 "Safe" | `t_cur < t_threshold` | — | 0 | Leakage (fast/slow decay), resets overheat-latch + Zone3 hysteresis |
| 2 "Anticipation" | `t_threshold <= t_cur < t_set`, without Zone3 hysteresis | — | 0 | Charging (except overheat-latch — see below) |
| 3 "Overheat" | `t_cur >= t_set` | `t_cur < t_set - THERMAL_ZONE3_HYSTERESIS` | Active | Charging from `error` (t_cur - t_set), overheat-latch + Zone3 hysteresis |

**Zone 3 hysteresis** (`THERMAL_ZONE3_HYSTERESIS = 2`): Zone 3 enters immediately when `t_cur >= t_set`, but exits only when `t_cur < t_set - 2`. While `THERMAL_FLAG_IN_ZONE3` is set, the Zone 2 branch is fully skipped. This prevents P jitter and integrator charging law switching when EMA filter fluctuates around the setpoint (699, 700, 699, 700...). The latch resets in Zone 1 (unconditionally) and in Zone 3 (when `t_cur < t_set - 2`).

### Overheat-latch (THERMAL_FLAG_OVERHEAT_LATCH)
Without considering temperature change direction (architecture without D-term) Zone 2 charged I constantly while `t_cur >= t_threshold` — including cases where temperature was already decreasing after actual overheat, just "passing through" the wide anticipation zone back into Zone 1. This caused unlimited I growth during cooling.

Solution — a bit flag latch (similar to `THERMAL_FLAG_FAST_LEAK`, without derivative):
- Zone 3 sets the latch (`state->flags |= THERMAL_FLAG_OVERHEAT_LATCH`) on first entry into actual overheat.
- While the latch is set, Zone 2 **does not charge** I, but slowly **leaks** it (`integral -= integral >> 8`, `THERMAL_OVERHEAT_LATCH_LEAK_SHIFT`, guaranteed to reach zero) — anticipation charge only makes sense on first approach to setpoint, not on departure after actual overheat. The leak (not simple hold) is needed for brightness to recover smoothly during cooling, not remaining frozen until hard `t_threshold` crossing.
- The leak is applied **every tick** with shift `>>8` — significantly gentler than Zone 1 slow-leak (`>>5`). On actual I range (0..511) this gives fixed speed ~1 unit/tick (~30/sec): smooth (without "staircase" in log), and one noisy ADC/EMA tick loses only ~1 unit of I — not critical. Tried gating the leak with same `ki_decimation_shift` as charging — resulted in stepped (not smooth) I correction every ~4.3 seconds.
- The leak is gated by **separate** `overheat_leak_decimation_shift` parameter (independent of `ki_decimation_shift` charging) — with shift `>>8` the actual I decay range always hits "minimum 1" floor, so leak speed stuck at ~30/sec and increasing `THERMAL_OVERHEAT_LATCH_LEAK_SHIFT` further would not slow it down. Without this gate, a short noise dip of 1 code (a few seconds) would quickly drain large I to 0 before temperature even resumed rising — the regulator "forgot" accumulated value after one noise spike and started a new cycle from zero.
- The leak is applied **every tick** with shift `>>8`, but gated by **own** `leak_tick_counter`, separate from `ki_tick_counter` charging — otherwise the first leak activation phase after Zone3→Zone2(latch) transition would depend on how many times charging had fired previously (random delay from 1 tick to full period), not on time spent in leak. Separate counters — correction found and suggested by user.
- Zone 1 resets the latch when returning to safe zone, arming Zone 2 for next heating cycle, and zeros `leak_tick_counter` so next overheat has predictable (limited to `2^overheat_leak_decimation_shift` ticks) first leak activation.

### Configuration (ThermalLimiterConfig_t)
```c
kp_left_shift;        // Proportional coefficient (shift bits, Zone 3)
ki_left_shift;        // Integral coefficient (shift bits, Zones 2/3)
ki_decimation_shift;  // "Fractional Ki": charging once per 2^N ticks (0..7)
t_anticipate_offset;  // Anticipation zone (ADC codes before t_set)
filter_shift;         // EMA temperature filter (shift bits)
max_drop_rate;        // Discharge speed (fast)
max_recovery_rate;    // Recovery speed (slow, 2-3x less than max_drop_rate)
overheat_leak_decimation_shift; // Zone2-latch leak once per 2^N ticks (independent of charging)
max_limiter;          // Limiter ceiling (0..255) — limits maximum dimming
```

### State (ThermalLimiterState_t) — 10 bytes
```c
filtered_temp;      // EMA-filtered temperature (int16_t)
integral;           // Dominant integrator (int16_t)
prev_limiter;       // Previous limiter value for slew rate (int16_t)
flags;              // Bit field: INITIALIZED, FAST_LEAK, ERROR, OVERHEAT_LATCH (uint8_t)
ki_tick_counter;    // Charging decimation gate counter (uint8_t)
leak_tick_counter;  // Leak decimation gate counter for Zone2-latch (uint8_t) + 1 byte padding
```

**Important:** `ki_tick_counter` and `leak_tick_counter` are separate counters. One common counter for two gates with different shifts (`ki_decimation_shift` charging and `overheat_leak_decimation_shift` leak) is only correct for average speed in long-term perspective — the phase of FIRST activation after Zone3→Zone2(latch) transition would depend on the residual counter value left by the other gate (from 1 tick to full `2^shift` period), not on time spent in leak. `leak_tick_counter` is reset to 0 when latch is released (Zone 1), so every new overheat entry has predictable (limited to `2^overheat_leak_decimation_shift` ticks) first leak activation.

### Algorithm (zero-multiplication)
1. **Failsafe**: `t_raw == 0 || t_raw >= 1023` → immediate bypass, PWM=0
2. **EMA Filter**: `filtered_temp += (t_raw - filtered_temp) >> filter_shift`
3. **Zones**: Calculate P and I based on t_cur vs t_threshold vs t_set
4. **Anti-Windup**: integrator clamp [0, user_pwm]
5. **Arbitration**: `raw_limiter = max(P, I)`
6. **Slew Rate**: Asymmetric filter (fast drop, slow recovery)

### API
```c
void ThermalLimiter_Init(ThermalLimiterState_t *state);
uint16_t ThermalLimiter_Update(const ThermalLimiterConfig_t *config,
                                ThermalLimiterState_t *state,
                                uint16_t t_raw, uint16_t t_set, uint16_t user_pwm);
```

### Current Configuration (limiters.c)
```c
#define T_SET 700u              // Temperature setpoint (ADC code)
.kp_left_shift = 5;            // P: error << 5 (strong response, Zone 3)
.ki_left_shift = 0;            // I: error << 0 (slow charging)
.ki_decimation_shift = 7;      // Charging once per 128 ticks (~4.3 sec)
.t_anticipate_offset = 30;     // Anticipation zone: [670, 700)
.filter_shift = 1;             // EMA: tau = 2 samples = 0.067 sec
.max_drop_rate = 3;            // Discharge speed (90/sec)
.max_recovery_rate = 1;        // Recovery speed (30/sec)
.overheat_leak_decimation_shift = 6; // Zone2-latch leak once per 64 ticks (~2.1 sec)
.max_limiter = 255;            // Limiter ceiling: full range (unlimited)
```

### Debug Output (main.c, tick_2hz)
```
T=raw F=filtered I=integrator L=limiter P=PWM
```

---

## Architectural Constraints (DO NOT VIOLATE WITHOUT EXPLICIT USER CONSENT)

These rules form the entire design — see `include/thermal_limiter.h` and `tz_new.md`:

1. **No `float` or `double`** — only integer/fixed-point calculations.
2. **No multiplication/division in hot loop** (`ThermalLimiter_Update` and everything it calls). All coefficients are bit shifts (`kp_left_shift`, `ki_left_shift`) via `<<`; all filters/leaks via `>>`. If `*` or `/` seems needed — need a shift or decimation-gate (see `ki_decimation_shift` / `charge_integral()` in `thermal_limiter.c`).
3. **State (`ThermalLimiterState_t`) — fields 16-bit or less.** `int32_t` is allowed only as *temporary* extension inside an expression (e.g., for safe `<<` before narrowing back to `int16_t`) — **never write back to structure**. RV32EC has 32-bit register file, extension is free in loops — 16-bit rule is about **memory volume**, not arithmetic cost.
4. **`Limiter = max(P, I)`, NEVER `P + I`.** This is "Intermittent Proportional + Dominant Integrator" architecture — do not "simplify" back to classic summing PI.
5. **Slew-rate filter (`max_drop_rate`/`max_recovery_rate`) — final authority** on brightness change speed. Any new term (e.g., derivative) must pass through this filter — never bypass it or write `state->prev_limiter` (or returned value) directly.
6. **Failsafe (`t_raw == 0 || t_raw >= 1023`)** — immediate bypass of same tick, force PWM=0 and output `"E_THERMAL_RUNAWAY\n"`. Do not add debounce/stability logic without explicit user request.

If any of the above needs to be violated — specify explicitly and get user confirmation.

---

## Resource Budget (check after every change)

`ThermalLimiterState_t` — 10 bytes (was 8, +2 after adding separate `leak_tick_counter`, see §"Overheat-latch"). Negligible against 2048 bytes RAM. When adding new field — first check if existing padding/spare bits can be reused (see `flags`/`THERMAL_FLAG_*` pattern), but do not sacrifice correctness for saving 1-2 bytes if bit reuse is insufficient (as in `leak_tick_counter` case — not enough free bits in `flags` for required shift range).

---

## Energy Saving Architecture

### MCU Operation Cycle
```
main loop:
  if (need_sleep):
    adc_stop()        → disable ADC
    tim2_stop()       → disable TIM2
    AWU_Sleep_Now()   → STANDBY → __WFI()
    [wake up]         → wakeup_source = WS_AWU/WS_BUTTON

  if (wakeup_source == WS_BUTTON):
    adc_init()        → restore ADC
    tim2_start()      → restore TIM2

  if (wakeup_source == WS_AWU):
    handle_aux()      → AUX command processing

  tick_160hz(): scan_ui1(), ss_scan()   → while not sleeping
  tick_30hz():  adc_read(), scan_limiters() → while not sleeping (thermal protection)
  tick_2hz():   (debug output)         → while not sleeping
  tick_5s():    handle_ee()            → while not sleeping
```

### Power Saving Rules
1. **adc_stop()** — disables ADC clock, sets PD2/PD3 to Hi-Z
2. **tim2_stop()** — disables TIM2 and its clock
3. **i2c_deinit()** — disables I2C1 and GPIO to Hi-Z
4. **AWU_Sleep_Now()** — enter STANDBY, `__WFI()` until wake
5. **AWU_ON()/AWU_OFF()** — macros to enable/disable AWU timer

---

## AWU (Auto Wake-Up)

### Registers (direct write)
- `PWR->AWUPSC` — prescaler (bits 3:0)
- `PWR->AWUWR` — window value (bits 5:0)
- `PWR->AWUCSR` — AWU control (bit 1: AWUEN)
- `PWR->CTLR` — PDDS (bit 1 = STANDBY mode)

### Wake-up
- **AWU timer** → EXTI Line9 → `AWU_IRQHandler` → `wakeup_source = WS_AWU`
- **Button** → EXTI Line3 → `EXTI7_0_IRQHandler` → `wakeup_source = WS_BUTTON`

### Important: AWU_IRQHandler
- `SystemInit()` is called **only** when `is_sleep_mode == true` (actual wake from STANDBY)
- If AWU fires while MCU is awake — `SystemInit()` is NOT called (otherwise it would break UART)

### AWU Delays
```c
AWU_15MS, AWU_30MS, AWU_60MS, AWU_120MS, AWU_250MS,
AWU_500MS, AWU_1S, AWU_2S, AWU_4S, AWU_8S, AWU_16S
```

### Time Formula
```
Time = (psc_value × win) / 128000 seconds
```

---

## TIM2 (System Timer)

### Configuration
- SYSCLK = 48 MHz, APB1 = 48 MHz
- PSC = 47 → timer clock = 1 MHz (1 tick = 1 µs)
- ATRLR = 999 → interrupt every 1000 ticks = 1 ms

### Usage
```c
tim2_start();           // Enable timer
tim2_stop();            // Disable (energy saving)

if (tick_160hz()) { }   // true every ~6 ms (UI, soft start)
if (tick_30hz())  { }   // true every ~33 ms (ADC + thermal protection)
if (tick_2hz())   { }   // true every 500 ms (debug)
if (tick_5s())    { }   // true every 5 seconds (data saving)
void tick_5s_reset(void);  // Reset 5s timer (prevents sleep during ramping)

extern volatile uint32_t tim2_ms;  // Global millisecond counter
```

---

## I2C (Direct Registers)

### Pins
- PC1 — SDA (AF_OD, Open-Drain)
- PC2 — SCL (AF_OD, Open-Drain)

### Registers (direct write)
- `I2C1->CTLR1` — control (PE, START, STOP, ACK, SWRST)
- `I2C1->CTLR2` — APB1 frequency (MHz)
- `I2C1->CKCFGR` — CCR + F/S mode
- `I2C1->OADDR1` — own address
- `I2C1->DATAR` — data register
- `I2C1->STAR1` — status (SB, ADDR, TXE, AF)
- `I2C1->STAR2` — status (BUSY, MSL)

### CCR Formula
```
Standard Mode:  CCR = PCLK1 / (2 × FI2C)
Fast Mode:      CCR = PCLK1 / (3 × FI2C)
```

### Important
- **i2c_deinit()** — mandatory before sleep (GPIO to Hi-Z)
- **i2c_stop()** waits for bus release (BUSY=0)
- **GPIO CFGLR**: `0x0F` = AF_OD 30MHz (SDK masks Mode via `& 0x0F`!)

---

## DAC7571 (I2C, 12-bit)

### Address
- 7-bit: `0x4C`
- 8-bit (write): `0x98`

### Data Format
```
[15:14] — mode (00=normal, 01=PD_1K, 10=PD_100K, 11=PD_HiZ)
[13:12] — reserved (00)
[11:0]  — 12-bit value (0x000..0xFFF)
```

### Functions
- `dac_pwrpin_init/on/off()` — power control (PD0)
- `dac_set_value(data)` — set 12-bit value
- `dac_set_100K()` — Power-Down 100kΩ (current saving)

---

## AUX LED Automaton

### Command Format (32 bits)
```
[2:0]   device    — channel: 1=RED, 2=GRN, 4=HBL
[3]     cycle     — 0=looped, 1=one-shot
[7:4]   count     — number of repeats (0..15)
[10:8]  psc0      — prescaler for 1st half-period (0..7)
[16:11] win0      — window for 1st half-period (0..63)
[19:17] psc1      — prescaler for 2nd half-period
[25:20] win1      — window for 2nd half-period
[28:26] brightness0 — brightness for 1st half-period
[31:29] brightness1 — brightness for 2nd half-period
```

### FSM (Finite State Machine)
```
PER_INIT → PER_FIRST (ON) → PER_SECOND (OFF) → PER_END → PER_FIRST or PER_NEXT
```

### System Array Commands
- `AC_STOP (0)` — stop processing
- `AC_REP_ATSTART (1)` — loop from beginning
- `AC_REP_ATSTEPS (2)` — loop from step

### Important
- `handle_aux()` returns `true` if there are active commands (AWU is restarted)
- `handle_aux()` returns `false` if all channels completed (AWU is NOT restarted)
- Elements with `psc=0, win=0` fire instantly — **always set non-zero values!**

---

## UART (Debug)

### Configuration
- Speed: **460800 baud**
- USART1: TX (PD5), RX (PD6)

### Usage
```c
EE_PRINTF("[TAG] message\r\n");  // Output via UART if debug enabled
```

### Performance Impact
- At 460800 baud: 1 symbol ≈ 21.7µs
- 35 symbols ≈ 0.76ms
- **UART does NOT block ramping** at this speed

### Configuration (config.h)
```c
#define WL_FLASH_DEBUG  // UART debug (mini_printf)
#define PRINT_ADC       // ADC value output
```

---

## Sizes
- Flash: 16000 bytes (last 256 bytes reserved for data)
- RAM: 2048 bytes

---

## Code Style

- **C99**, `stdint.h` fixed width everywhere (`int16_t`, `uint16_t`, `uint8_t`) — no `int`/`long`/`short` for ADC/PWM/state.
- **Comments — loading documentation**, not decoration: every non-obvious shift, clamp, or zone boundary has a comment explaining *why*, not just *what*. Comment density matches code additions.
- **Naming**: `snake_case` for struct fields and local variables, `PascalCase` for public functions (`ThermalLimiter_Init`, `ThermalLimiter_Update`), `THERMAL_*` prefix for macros/constants.
- **Language**: Russian for comments in `main.c` (hardware, ADC) and `TUNING.md`. English for `thermal_limiter.h`/`.c` (algorithm/library) and this file. Match the language of the file being edited — do not mix in one block.
- Direct register writes: `RCC->APB2PCENR |= RCC_AFIOEN`
- Header files with `#ifndef` guards
- Timeouts in all wait loops (while) — MCU can hang

---

## When Changing Configurable Behavior

If a change affects what a `ThermalLimiterConfig_t` field means, how a zone boundary behaves, or the debug log format in `main.c` — update `TUNING.md` in the same change. This is the tuning reference per field and will become outdated without updates.

---

## Known, Intentionally Unfixed Compromises

Recorded in previous review, listed so they are not "reopened" without asking:

- **No debounce on failsafe ADC-rail check** (specification requires "immediately"). Add only if user reports actual ADC noise on rails and requests it.
- **No D-term (derivative).** Intentionally discarded — derivative amplifies quantized dither of EMA filter and injects exactly those bad, unbounded instant spikes that slew filter and decimation-gated integrator are designed to avoid. Do not add without explicit user request after reminder of this compromise.

---

## Rules for AI Assistants

### What is FORBIDDEN (critically important!)
1. **Do not use STM32 information** — CH32V003 ≠ STM32 (different registers, different behavior)
2. **Do not copy STM32 code** — even if registers have same names
3. **Do not use SDK helpers** (`EXTI_Init()`, `NVIC_Init()`, `I2C_Init()`) — project uses direct registers (except HAL in `main.c`)
4. **Do not add dynamic memory** (malloc/free) — no heap
5. **Do not use printf** — only `EE_PRINTF()` via mini_printf
6. **Do not forget to disable peripherals** when entering sleep
7. **Do not violate architectural constraints** thermal_limiter without explicit user consent

### What is ALLOWED
1. **Modify registers directly** — project uses direct register writes
2. **Use `EE_PRINTF()`** for debug output
3. **Add `#include "ch32v00x.h"`** for register access
4. **Use `AWU_ON()/AWU_OFF()`** macros
5. **Use `ON_POWER()/OFF_POWER()`** for DAC power control

### Power Saving Priority
1. Always check that peripherals are disabled before sleep
2. Use `adc_stop()`, `tim2_stop()`, `i2c_deinit()` before STANDBY
3. Do not leave AWU enabled unnecessarily

### GPIO CFGLR (common error!)
SDK masks Mode via `& 0x0F` before writing to CFGLR:
```c
// SDK does: currentmode = Mode & 0x0F
// AF_OD (0x1C) → 0x0C, then |= Speed (0x03) → 0x0F
// Do NOT write 0x1C directly to CFGLR — it will affect adjacent pin!
#define GPIO_CFG_AF_OD_30M  0x0F  // Correct