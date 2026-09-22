/* ============================================================
 * wl_flash_config.h — compile-time configuration for WL_FLASH
 *
 * Every macro below is wrapped in #ifndef, so all of them can be
 * overridden from platformio.ini:
 *
 *   build_flags =
 *       -DWL_PAGE_COUNT=8
 *       -DWL_RECORD_SIZE=20
 *       -DWL_SYSCLK_HZ=48000000
 *       -DWL_FLASH_DEBUG
 *
 * IMPORTANT: the EEPROM region lives at the end of Flash and must be
 * excluded from the firmware link image:
 *
 *   board_upload.maximum_size = <flash size> - WL_PAGE_COUNT * 64
 *
 * The bundled extra script (scripts/reserve_eeprom.py) verifies this at
 * build time and aborts the build when the value is missing or wrong.
 * ============================================================ */
#ifndef WL_FLASH_CONFIG_H
#define WL_FLASH_CONFIG_H

/* ─── Physical Flash geometry ───────────────────────────────────── */

/* Start of on-chip Flash in the address space. */
#ifndef WL_FLASH_BASE_ADDR
#define WL_FLASH_BASE_ADDR 0x08000000UL
#endif

/* Total on-chip Flash size in bytes.
 * CH32V003F4P6 = 16384. CH32V003F8P6 = 32768. */
#ifndef WL_FLASH_TOTAL_SIZE
#define WL_FLASH_TOTAL_SIZE 16384UL
#endif

/* Size of the smallest erasable unit. The CH32V00x "fast page erase"
 * granularity is 64 bytes; do not change unless the part differs. */
#ifndef WL_FLASH_PAGE_SIZE
#define WL_FLASH_PAGE_SIZE 64UL
#endif

/* ─── EEPROM region ─────────────────────────────────────────────── */

/* Number of 64-byte pages reserved for the record ring.
 * Minimum 2 — one page must always survive while another is erased. */
#ifndef WL_PAGE_COUNT
#define WL_PAGE_COUNT 4
#endif

/* End address of the EEPROM region (exclusive). Defaults to the end of
 * Flash, which is where the region normally belongs. */
#ifndef WL_REGION_END
#define WL_REGION_END (WL_FLASH_BASE_ADDR + WL_FLASH_TOTAL_SIZE)
#endif

/* ─── Record layout ─────────────────────────────────────────────── */

/* Size of one record slot in bytes. Must be a multiple of 4, at least 8,
 * and small enough for 2 slots to fit in a page (i.e. at most 32).
 *
 * Header overhead is 5 bytes (seq + version) plus the CRC width, so the
 * user payload is WL_RECORD_SIZE - 5 - WL_CRC_SIZE bytes. */
#ifndef WL_RECORD_SIZE
#define WL_RECORD_SIZE 16
#endif

/* CRC width in bits: 16 or 32.
 *
 * An 8-byte record has no room for a 32-bit CRC next to the header, so it
 * defaults to CRC-16. Larger records default to CRC-32 but may be forced
 * to CRC-16 with -DWL_CRC_BITS=16 to buy 2 extra payload bytes. */
#ifndef WL_CRC_BITS
#if WL_RECORD_SIZE < 12
#define WL_CRC_BITS 16
#else
#define WL_CRC_BITS 32
#endif
#endif

/* On-flash format version, stored in every record. Bump this in the
 * application when the meaning of the user payload changes; wl_load()
 * then reports WL_BAD_VERSION instead of handing over stale data. */
#ifndef WL_FORMAT_VERSION
#define WL_FORMAT_VERSION 1
#endif

/* ─── Behaviour ─────────────────────────────────────────────────── */

/* Consecutive wl_commit() failures tolerated before the dirty flag is
 * latched into a permanent error. Without a bound, a worn-out or
 * write-protected Flash would be hammered with erase cycles on every
 * pass of the main loop. Cleared by wl_init() or wl_load(). */
#ifndef WL_MAX_COMMIT_RETRY
#define WL_MAX_COMMIT_RETRY 3
#endif

/* ─── Option Bytes store (opt-in) ───────────────────────────────── */

/* Define WL_OB_ENABLE to add the wl_ob_* API (src/wl_flash_ob.h): a small
 * store for rarely-changed parameters in the Option Bytes region, in
 * addition to the record ring. Off by default, so existing builds are
 * completely unaffected. */

#ifndef WL_OB_ENABLE
#define WL_OB_ENABLE 0
#endif

#if WL_OB_ENABLE

/* Number of user bytes kept in the Option Bytes area. The datasheet
 * documents two Data bytes; anything above 4 sits past the register block
 * and its persistence is not covered by the datasheet — verify against
 * the part before raising this. Hard cap 16 (static asserted in
 * wl_flash_ob.h). */
#ifndef WL_OB_USER_BYTES
#define WL_OB_USER_BYTES 4
#endif

/* Consecutive wl_ob_flush() failures tolerated before it stops writing
 * and keeps reporting failure until the data changes again. Without a
 * bound, an unusable OB region would be erase-cycled on every pass of
 * the main loop. Cleared by wl_ob_mark_dirty(). */
#ifndef WL_OB_MAX_FLUSH_RETRY
#define WL_OB_MAX_FLUSH_RETRY 3
#endif

/* Base of the Option Bytes block. Do not change on CH32V00x. */
#ifndef WL_OB_BASE_ADDR
#define WL_OB_BASE_ADDR 0x1FFFF800UL
#endif

/* Start of the user area, byte i stored at WL_OB_USER_ADDR + i * 2 with
 * the value in the low half-word byte — the controller writes the one's
 * complement into the high byte automatically. The default sits past
 * the six standard registers (RDPR, USER, Data0, Data1, WRPR0, WRPR1).
 * Byte 0..3 at base+16 keeps the on-flash layout identical to the
 * standalone OB drivers this module replaced, so values written by them
 * load without migration. */
#ifndef WL_OB_USER_ADDR
#define WL_OB_USER_ADDR (WL_OB_BASE_ADDR + 16UL)
#endif

/* Hardware caveat: the OPTER erase clears the entire Option Bytes page,
 * including the standard registers — read protection, boot settings,
 * write protection. The back end therefore saves and reprograms them on
 * every flush, and a failed or interrupted one leaves the whole area
 * undefined until the next successful flush. Because USER=0xFF changes
 * IWDG and NRST behaviour after a power-up glitch, keep only parameters
 * that can safely fall back to defaults here. */

/* Timeout for OB operations is the existing WL_FLASH_TIMEOUT above; no
 * separate tunable. */

#endif /* WL_OB_ENABLE */

/* ─── Clock / timing ────────────────────────────────────────────── */

/* Core clock in Hz. Used to rebase the Flash controller wait timeout
 * onto real time; it does not configure the clock itself.
 *
 * The noneos-sdk builds boot at the silicon reset default (HSI, no PLL)
 * whatever the board manifest says, which on CH32V00x is 24 MHz — so
 * when nothing below applies, that is the assumed clock.
 *
 * Precedence:
 *   1. an explicit -DWL_SYSCLK_HZ always wins;
 *   2. a vendor SYSCLK_FREQ_* define (from system_ch32v00x.c) maps to
 *      the clock it names. Deliberately literal, not the macro value:
 *      a bare -DSYSCLK_FREQ_48MHZ_HSI without a value would otherwise
 *      default to 1 and break the arithmetic;
 *   3. the HSI reset default, 24 MHz.
 *
 * Declaring a clock higher than the real one only makes the timeout
 * longer, never shorter — erring high is the safe direction. */
#ifndef WL_SYSCLK_HZ
#if defined(SYSCLK_FREQ_48MHZ_HSI) || defined(SYSCLK_FREQ_48MHz_HSE)
#define WL_SYSCLK_HZ 48000000UL
#elif defined(SYSCLK_FREQ_24MHZ_HSI) || defined(SYSCLK_FREQ_24MHz_HSE) \
    || defined(SYSCLK_FREQ_HSI)
#define WL_SYSCLK_HZ 24000000UL
#elif defined(SYSCLK_FREQ_8MHz_HSI) || defined(SYSCLK_FREQ_8MHz_HSE) \
    || defined(SYSCLK_FREQ_HSI_LP)
#define WL_SYSCLK_HZ 8000000UL
#else
#define WL_SYSCLK_HZ 24000000UL
#endif
#endif

/* Busy-wait budget for the Flash controller, in microseconds. Defaults
 * to 100 ms — roughly 20x the worst-case ~5 ms page erase — which at
 * 48 MHz works out to ~2.4 M loop iterations, the same order as the
 * literal 2 M the library always used. */
#ifndef WL_FLASH_TIMEOUT_US
#define WL_FLASH_TIMEOUT_US 100000UL
#endif

/* Iterations to wait for the Flash controller before declaring a
 * hardware failure. Roughly 2 cycles per iteration. When not overridden
 * it is derived from WL_FLASH_TIMEOUT_US and WL_SYSCLK_HZ entirely by
 * preprocessor constant folding — no run-time division (RV32EC has no
 * divider), no casts — so it stays usable in #if and _Static_assert.
 *
 * An explicit -DWL_FLASH_TIMEOUT still wins (backward compatibility). */
#ifndef WL_FLASH_TIMEOUT
#define WL_FLASH_TIMEOUT \
    ((WL_FLASH_TIMEOUT_US * (WL_SYSCLK_HZ / 1000000UL) + 1UL) / 2UL)
#endif

/* Compile-time sanity. The clock must lie in 1 MHz..400 MHz and the
 * budget in 10 us..10 s — the upper bounds also keep the 32-bit product
 * in the derivation above from overflowing. */
#if WL_SYSCLK_HZ < 1000000UL || WL_SYSCLK_HZ > 400000000UL
#error "WL_SYSCLK_HZ is outside the supported range (1 MHz..400 MHz)"
#endif

#if WL_FLASH_TIMEOUT_US < 10UL || WL_FLASH_TIMEOUT_US > 10000000UL
#error "WL_FLASH_TIMEOUT_US is outside the supported range (10 us..10 s)"
#endif

#if WL_FLASH_TIMEOUT < 1UL
#error "WL_FLASH_TIMEOUT would be zero - raise WL_FLASH_TIMEOUT_US or WL_SYSCLK_HZ"
#endif

/* Define WL_FLASH_DEBUG to get printf() tracing. Undefined by default,
 * in which case every trace statement compiles to nothing.
 *
 * Deliberately not named DEBUG: the vendor debug.h auto-defines DEBUG to
 * a nonzero value, which would silently force tracing on. */

/* Define WL_FLASH_HAL_MOCK to unhook the CH32V back end and supply the
 * HAL entry points yourself. Used by the host test suite. */

/* ─── Derived geometry — do not override ────────────────────────── */

/* Kept free of casts and of <stdint.h> so that these expressions stay
 * usable from preprocessor arithmetic and _Static_assert. All operands
 * carry a UL suffix, so every product below is unsigned long. */

#define WL_REGION_SIZE  (WL_PAGE_COUNT * WL_FLASH_PAGE_SIZE)
#define WL_REGION_START (WL_REGION_END - WL_REGION_SIZE)

#endif /* WL_FLASH_CONFIG_H */
