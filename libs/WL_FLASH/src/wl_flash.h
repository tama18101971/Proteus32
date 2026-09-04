/* ============================================================
 * wl_flash.h — wear-levelling record store in on-chip Flash
 *
 * A fixed-size record is written to a ring of 64-byte Flash pages. Every
 * record carries a sequence number and a CRC; the newest record that
 * passes both checks wins at load time. Records are appended slot by
 * slot, and a page is erased only when the ring rotates back onto it, so
 * erase wear spreads across all pages in the region.
 *
 * Power-loss behaviour: an interrupted write leaves a record with a
 * broken CRC, which is skipped, so the previous record stays current.
 * A record is never overwritten in place and a page is never erased
 * while it holds the newest record.
 *
 * Usage:
 *
 *     typedef struct { uint8_t brightness; int8_t trim; } my_cfg_t;
 *     WL_ASSERT_PAYLOAD_FITS(my_cfg_t);
 *     #define CFG WL_DATA_AS(my_cfg_t)
 *
 *     if (wl_load() != WL_OK) {
 *         wl_init();                  // no usable record — factory reset
 *         CFG->brightness = 128;
 *     }
 *     CFG->trim = -3;
 *     wl_mark_dirty();                // may also be called from an ISR
 *     ...
 *     wl_commit_if_dirty();           // from the main loop
 * ============================================================ */
#ifndef WL_FLASH_H
#define WL_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wl_flash_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Derived layout ────────────────────────────────────────────── */

#if WL_CRC_BITS == 16
typedef uint16_t wl_crc_t;
#define WL_CRC_SIZE 2
#elif WL_CRC_BITS == 32
typedef uint32_t wl_crc_t;
#define WL_CRC_SIZE 4
#else
#error "WL_CRC_BITS must be 16 or 32"
#endif

/* Header overhead: seq(4) + version(1) + crc(WL_CRC_SIZE). */
#define WL_HEADER_SIZE  (5 + WL_CRC_SIZE)

/* Bytes of user payload available in one record. */
#define WL_DATA_SIZE    (WL_RECORD_SIZE - WL_HEADER_SIZE)

/* Records per page. Any remainder is dead space at the end of the page;
 * the ring never addresses it. */
#define WL_SLOTS_PER_PAGE (WL_FLASH_PAGE_SIZE / WL_RECORD_SIZE)

/* Total number of addressable record slots in the region. */
#define WL_TOTAL_SLOTS    (WL_PAGE_COUNT * WL_SLOTS_PER_PAGE)

/* ─── Compile-time validation ───────────────────────────────────── */

_Static_assert(WL_RECORD_SIZE % 4 == 0,
               "WL_RECORD_SIZE must be a multiple of 4");
_Static_assert(WL_FLASH_PAGE_SIZE % 4 == 0,
               "WL_FLASH_PAGE_SIZE must be a multiple of 4");
_Static_assert(WL_DATA_SIZE >= 1,
               "WL_RECORD_SIZE leaves no room for user payload; "
               "raise WL_RECORD_SIZE or set WL_CRC_BITS=16");
_Static_assert(WL_SLOTS_PER_PAGE >= 2,
               "WL_RECORD_SIZE too large; a page must hold at least 2 records");
_Static_assert(WL_PAGE_COUNT >= 2,
               "WL_PAGE_COUNT must be at least 2 for wear levelling");

/* Region must lie inside physical Flash and be page-aligned at both ends.
 * Without these, a typo in WL_REGION_END or WL_FLASH_TOTAL_SIZE would
 * silently produce a build that erases arbitrary Flash. */
_Static_assert(WL_REGION_END <= WL_FLASH_BASE_ADDR + WL_FLASH_TOTAL_SIZE,
               "EEPROM region ends past the end of Flash");
_Static_assert(WL_REGION_START >= WL_FLASH_BASE_ADDR,
               "EEPROM region starts before the beginning of Flash");
_Static_assert(WL_REGION_START % WL_FLASH_PAGE_SIZE == 0,
               "EEPROM region start must be page-aligned");
_Static_assert(WL_REGION_END % WL_FLASH_PAGE_SIZE == 0,
               "EEPROM region end must be page-aligned");

/* ─── Record layout ─────────────────────────────────────────────── */

/**
 * @brief  On-flash record.
 *
 * | offset             | size         | field   |
 * |--------------------|--------------|---------|
 * | 0                  | 4            | seq     |
 * | 4                  | WL_DATA_SIZE | data    |
 * | 4 + WL_DATA_SIZE   | 1            | version |
 * | WL_RECORD_SIZE - n | n            | crc     |
 *
 * The CRC covers every byte before the crc field itself.
 *
 * `data` deliberately sits at offset 4 rather than following `version`:
 * records start on 4-byte boundaries, so the payload is 4-byte aligned
 * and WL_DATA_AS() may hand out a pointer to any type of alignment 4 or
 * less. RV32EC has no hardware support for misaligned loads.
 *
 * The struct is not packed. For every supported combination of
 * WL_RECORD_SIZE and WL_CRC_BITS the natural layout already fills the
 * record exactly, so the compiler is free to use word-wide accesses; the
 * assertions below fail the build if that ever stops holding.
 */
typedef struct __attribute__((aligned(4))) {
    uint32_t seq;                 /**< Monotonic write counter. */
    uint8_t  data[WL_DATA_SIZE];  /**< User payload, 4-byte aligned. */
    uint8_t  version;             /**< WL_FORMAT_VERSION at write time. */
    wl_crc_t crc;                 /**< CRC over the preceding bytes. */
} wl_record_t;

_Static_assert(sizeof(wl_record_t) == WL_RECORD_SIZE,
               "wl_record_t size does not match WL_RECORD_SIZE");
_Static_assert(__alignof__(wl_record_t) == 4,
               "wl_record_t must be 4-byte aligned");
_Static_assert(offsetof(wl_record_t, data) == 4,
               "payload must be 4-byte aligned for WL_DATA_AS()");
_Static_assert(offsetof(wl_record_t, crc) == WL_RECORD_SIZE - WL_CRC_SIZE,
               "crc must be the last field of the record");

/* ─── Status codes ──────────────────────────────────────────────── */

typedef enum {
    WL_OK = 0,          /**< Operation succeeded. */
    WL_EMPTY,           /**< Region blank — no record has ever been written. */
    WL_CORRUPT,         /**< Records present but none passed its CRC. */
    WL_BAD_VERSION,     /**< Newest valid record has a different version. */
    WL_HW_ERROR,        /**< Flash controller error, timeout, or verify
                             mismatch. The previous record is untouched. */
    WL_NO_SPACE,        /**< No writable slot found — region unusable. */
    WL_RETRY_EXHAUSTED, /**< WL_MAX_COMMIT_RETRY consecutive failures;
                             committing is latched off until wl_init() or
                             wl_load() clears it. */
    WL_BUSY             /**< Re-entrant call rejected. */
} wl_status_t;

/* ─── Public API ────────────────────────────────────────────────── */

/**
 * @brief  Reset the RAM working copy to factory defaults.
 *
 * Zeroes the payload, stamps the current format version, and clears the
 * dirty flag and the retry counter. Does not touch Flash: the reset is
 * persisted by the next successful wl_commit().
 */
void wl_init(void);

/**
 * @brief  Load the newest usable record from Flash into the working copy.
 *
 * Scans every slot, keeps the record with the highest sequence number
 * whose CRC checks out, and positions the write cursor after it.
 *
 * The working copy is modified only on WL_OK. On any other result it is
 * left untouched, so a wl_init() performed beforehand survives.
 *
 * @return WL_OK, WL_EMPTY, WL_CORRUPT or WL_BAD_VERSION.
 */
wl_status_t wl_load(void);

/**
 * @brief  Append the working copy to Flash as a new record.
 *
 * Assigns the next sequence number, computes the CRC, programs the next
 * writable slot (erasing a page when the ring rotates onto it), and
 * verifies the result. Clears the dirty flag on success.
 *
 * On failure the previously stored record remains current and the cursor
 * is left so that the next attempt skips the failed slot.
 *
 * Not re-entrant. Blocks for the duration of a page erase when the ring
 * rotates onto a used page. Call from the main loop, not from an
 * interrupt handler — see wl_mark_dirty().
 *
 * @return WL_OK, WL_HW_ERROR, WL_NO_SPACE or WL_BUSY.
 */
wl_status_t wl_commit(void);

/**
 * @brief  Request a commit. Safe to call from an interrupt handler.
 *
 * Sets a flag and returns; no Flash access. The write happens on the next
 * wl_commit_if_dirty() from the main loop.
 */
void wl_mark_dirty(void);

/**
 * @brief  Whether a commit is pending.
 */
bool wl_is_dirty(void);

/**
 * @brief  Commit if the dirty flag is set. Intended for the main loop.
 *
 * The flag is cleared before programming starts, so a wl_mark_dirty()
 * raised by an interrupt during the write is not lost. After
 * WL_MAX_COMMIT_RETRY consecutive failures this stops retrying and
 * returns WL_RETRY_EXHAUSTED until wl_init() or wl_load() is called.
 *
 * @return WL_OK when nothing was pending or the commit succeeded,
 *         otherwise the failing status.
 */
wl_status_t wl_commit_if_dirty(void);

/**
 * @brief  Pointer to the mutable user payload of the working copy.
 * @return Pointer to WL_DATA_SIZE writable bytes, 4-byte aligned minus
 *         the 5-byte header offset.
 */
void *wl_data(void);

/**
 * @brief  Sequence number of the working copy.
 * @return 0 before the first successful wl_load() or wl_commit().
 */
uint32_t wl_seq(void);

/* ─── Payload helpers ───────────────────────────────────────────── */

/** Typed access to the payload: `WL_DATA_AS(my_cfg_t)->field`. */
#define WL_DATA_AS(type) ((type *)wl_data())

/** Compile-time guard that a payload type fits in one record. */
#define WL_ASSERT_PAYLOAD_FITS(type)     \
    _Static_assert(sizeof(type) <= WL_DATA_SIZE, \
                   #type " does not fit in WL_DATA_SIZE; raise WL_RECORD_SIZE")

/* ─── Test hook ─────────────────────────────────────────────────── */

#ifdef WL_FLASH_TESTING
/**
 * @brief  Return all module state to power-on defaults.
 *
 * Only compiled when WL_FLASH_TESTING is defined. The host test suite
 * needs it because the state is file-static and each test case has to
 * start from a cold boot.
 */
void wl_test_reset_state(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* WL_FLASH_H */
