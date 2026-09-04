/* ============================================================
 * wl_flash_hal.h — hardware abstraction for WL_FLASH
 *
 * Four primitives isolate every access to physical Flash. The record
 * ring algorithm in wl_flash.c uses nothing else, which lets the whole
 * state machine run on a host with a RAM-backed Flash model.
 *
 * Back ends:
 *   wl_flash_hal_ch32v.c   CH32V00x Flash controller (default)
 *   test/wl_flash_mock.c   RAM model with fault injection (native)
 *
 * Define WL_FLASH_HAL_MOCK to disable the CH32V back end and provide
 * these functions yourself.
 *
 * Contract for all four: `addr` and `len` are validated by the caller.
 * Implementations may assume `addr` lies inside the configured EEPROM
 * region and that `len` respects the alignment noted per function.
 * ============================================================ */
#ifndef WL_FLASH_HAL_H
#define WL_FLASH_HAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Test whether a Flash range holds only erased (0xFF) bytes.
 * @param  addr  Start address, 4-byte aligned.
 * @param  len   Length in bytes, multiple of 4.
 * @return true when every byte in the range reads as 0xFF.
 */
bool wl_hal_is_erased(uint32_t addr, uint32_t len);

/**
 * @brief  Copy a Flash range into RAM.
 * @param  addr  Start address, 4-byte aligned.
 * @param  dst   Destination buffer, at least @p len bytes.
 * @param  len   Length in bytes.
 */
void wl_hal_read(uint32_t addr, void *dst, uint32_t len);

/**
 * @brief  Program a Flash range from RAM.
 *
 * The target range must be fully erased. Implementations must refuse to
 * program a half-word that is not 0xFFFF: on CH32V00x, programming an
 * already-written cell can corrupt it.
 *
 * @param  addr  Start address, 4-byte aligned.
 * @param  src   Source buffer.
 * @param  len   Length in bytes, multiple of 2.
 * @return true on success, false on any hardware error.
 */
bool wl_hal_program(uint32_t addr, const void *src, uint32_t len);

/**
 * @brief  Compare a Flash range against RAM.
 * @param  addr  Start address.
 * @param  src   Expected contents.
 * @param  len   Length in bytes.
 * @return true when Flash matches @p src byte for byte.
 */
bool wl_hal_verify(uint32_t addr, const void *src, uint32_t len);

/**
 * @brief  Erase one Flash page.
 * @param  addr  Page-aligned start address of the page to erase.
 * @return true on success, false on any hardware error.
 */
bool wl_hal_erase_page(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* WL_FLASH_HAL_H */
