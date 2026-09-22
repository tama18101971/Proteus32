/* ============================================================
 * wl_flash_ob.h — small store for the CH32V00x Option Bytes area
 *
 * Complements the record ring with an optional place for parameters
 * that change rarely: calibration offsets, counters, the occasional
 * flag. Two hardware facts shape it:
 *
 *   - the OPTER erase wipes the entire Option Bytes page, standard
 *     registers included, so a flush is erase + full reprogram — there
 *     is no room for a wear-levelling ring here;
 *   - an interrupted or failing flush leaves the whole area undefined,
 *     so the data must be re-derivable: the application stores its own
 *     magic byte and applies defaults when the magic is gone.
 *
 * Layout on flash: user byte i at WL_OB_USER_ADDR + i * 2, value in the
 * low half-word byte; the controller writes the one's complement into
 * the high half on its own.
 *
 * Usage:
 *
 *     typedef struct { uint8_t magic; int8_t tc; } my_ob_t;
 *     WL_OB_ASSERT_FITS(my_ob_t);
 *     #define OB WL_OB_DATA(my_ob_t)
 *
 *     wl_ob_init();
 *     if (OB->magic != 0xA5) {          // nothing usable stored
 *         OB->tc = 0;
 *         OB->magic = 0xA5;
 *     }
 *     wl_ob_mark_dirty();               // after any change
 *     ...
 *     wl_ob_flush();                    // from the main loop
 *
 * Not re-entrant, and wl_ob_flush() must not be interleaved with
 * wl_commit() from different contexts — both drive the same Flash
 * controller. Call both from one main-loop context.
 * ============================================================ */
#ifndef WL_FLASH_OB_H
#define WL_FLASH_OB_H

#include <stdbool.h>
#include <stdint.h>

#include "wl_flash_config.h"

#if WL_OB_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Compile-time validation ───────────────────────────────────── */

_Static_assert(WL_OB_USER_BYTES >= 1,
               "WL_OB_USER_BYTES must be at least 1");
_Static_assert(WL_OB_USER_BYTES <= 16,
               "WL_OB_USER_BYTES is capped at 16; persistence past the "
               "datasheet area is unverified — check the part first");
_Static_assert(WL_OB_USER_ADDR % 2 == 0,
               "WL_OB_USER_ADDR must be half-word aligned");
_Static_assert(WL_OB_USER_ADDR >= WL_OB_BASE_ADDR + 12,
               "WL_OB_USER_ADDR must sit past the six standard Option "
               "Byte registers (WRPR1 ends at WL_OB_BASE_ADDR + 11)");

/* ─── Status codes ──────────────────────────────────────────────── */

typedef enum {
    WL_OB_OK = 0,   /**< Data programmed and verified. */
    WL_OB_IDLE,     /**< Nothing dirty — no Flash access happened. */
    WL_OB_FAIL      /**< Attempt failed, or retries are exhausted. */
} wl_ob_status_t;

/* ─── Public API ────────────────────────────────────────────────── */

/**
 * @brief  Load the stored user bytes into the RAM cache.
 *
 * Also clears the dirty flag and the retry counter. No return value:
 * whether the loaded bytes are valid is the application's business —
 * check the magic byte through WL_OB_DATA() and write defaults if it
 * is not there.
 */
void wl_ob_init(void);

/**
 * @brief  Pointer to the mutable user data cache.
 * @return Pointer to WL_OB_USER_BYTES writable bytes.
 */
void *wl_ob_data(void);

/**
 * @brief  Whether a flush is pending.
 */
bool wl_ob_is_dirty(void);

/**
 * @brief  Request a flush. Safe to call from an interrupt handler.
 *
 * Sets a flag and returns; no Flash access. The write happens on the
 * next wl_ob_flush() from the main loop, and also resets the retry
 * counter — a failed flush is retried as long as the caller keeps
 * touching the data.
 */
void wl_ob_mark_dirty(void);

/**
 * @brief  Program the cache into the Option Bytes area if dirty.
 *
 * A dirty flag without a real change costs no erase cycle: when the
 * area already holds the cache contents the flush only confirms it.
 *
 * @return WL_OB_IDLE when nothing is pending, WL_OB_OK on a verified
 *         write or a confirmed match, WL_OB_FAIL on a failed attempt or
 *         once WL_OB_MAX_FLUSH_RETRY consecutive failures are exhausted.
 *         On WL_OB_FAIL the stored data is undefined; nothing is
 *         written until the flag is cleared by another
 *         wl_ob_mark_dirty().
 */
wl_ob_status_t wl_ob_flush(void);

/* ─── Cache helpers ─────────────────────────────────────────────── */

/** Typed access to the cache: `WL_OB_DATA(my_ob_t)->field`. */
#define WL_OB_DATA(type) ((type *)wl_ob_data())

/** Compile-time guard that the Option Bytes data type fits. */
#define WL_OB_ASSERT_FITS(type)              \
    _Static_assert(sizeof(type) <= WL_OB_USER_BYTES, \
                   #type " does not fit in WL_OB_USER_BYTES")

/* ─── Test hook ─────────────────────────────────────────────────── */

#ifdef WL_FLASH_TESTING
/**
 * @brief  Return all module state to power-on defaults.
 *
 * Only compiled when WL_FLASH_TESTING is defined. The state is
 * file-static and each test case has to start from a cold boot.
 */
void wl_ob_test_reset_state(void);
#endif

#ifdef __cplusplus
}
#endif

#else /* !WL_OB_ENABLE */

/* An empty header is fine, but consumers may include this without
 * checking the gate; keep nothing here to break. */

#endif /* WL_OB_ENABLE */

#endif /* WL_FLASH_OB_H */
