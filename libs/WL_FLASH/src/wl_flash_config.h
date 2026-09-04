/* ============================================================
 * wl_flash_config.h — compile-time configuration for WL_FLASH
 *
 * Every macro below is wrapped in #ifndef, so all of them can be
 * overridden from platformio.ini:
 *
 *   build_flags =
 *       -DWL_PAGE_COUNT=8
 *       -DWL_RECORD_SIZE=20
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

/* Iterations to wait for the Flash controller before declaring a
 * hardware failure. Roughly 2 cycles per iteration; the default is
 * several orders of magnitude above the worst-case erase time. */
#ifndef WL_FLASH_TIMEOUT
#define WL_FLASH_TIMEOUT 2000000UL
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
