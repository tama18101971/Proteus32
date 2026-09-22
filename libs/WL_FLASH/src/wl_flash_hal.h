/* ============================================================
 * wl_flash_hal.h — hardware abstraction for WL_FLASH
 *
 * Four primitives isolate every access to physical Flash. The record
 * ring algorithm in wl_flash.c uses nothing else, which lets the whole
 * state machine run on a host with a RAM-backed Flash model.
 *
 * When WL_OB_ENABLE is on, an OB pair is added: wl_hal_ob_read() and
 * wl_hal_ob_program() carry the whole Option Bytes sequence, so the
 * wl_flash_ob.c policy layer also stays hardware-free.
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

/*
 * Option Bytes primitives. Only referenced by wl_flash_ob.c, which is
 * compiled only under WL_OB_ENABLE — a back end may provide them
 * unconditionally, but need not.
 */

/**
 * @brief  Read user bytes from the Option Bytes area.
 *
 * User byte i sits at WL_OB_USER_ADDR + i * 2 with the value in the low
 * byte of the half-word; the stride is handled inside the
 * implementation. On flash each cell stores the value plus the
 * hardware-written one's complement in the high half — reading the
 * value byte only is correct.
 *
 * @param  dst  Destination buffer, at least @p len bytes.
 * @param  len  Number of user bytes, at most WL_OB_USER_BYTES.
 */
void wl_hal_ob_read(uint8_t *dst, uint32_t len);

/**
 * @brief  Reprogram the whole Option Bytes page with new user bytes.
 *
 * One atomic-by-contrast sequence covering everything destructive:
 * mask interrupts, unlock the controller, save the six standard
 * registers (RDPR, USER, Data0, Data1, WRPR0, WRPR1), erase the Option
 * Bytes page with OPTER, reprogram the saved registers, program
 * @p len user half-words (value in the low byte; the controller fills
 * the high half with its complement) at WL_OB_USER_ADDR + i * 2, lock,
 * restore interrupts, then read the user bytes back and compare.
 * Returns false on any failure, including a
 * verify mismatch caught by the internal read-back.
 *
 * On false the previous contents of the Option Bytes area are
 * undefined — possibly erased and only partially reprogrammed. The
 * caller is expected to treat this as data loss: on the next
 * wl_ob_init() the application's own magic/default check catches it.
 *
 * Blocks for the duration of an OB erase with interrupts masked, so it
 * belongs in main-loop context, never in an interrupt handler.
 *
 * @param  src  Source buffer of @p len user bytes.
 * @param  len  Number of user bytes, at most WL_OB_USER_BYTES.
 * @return true on success, false on any hardware error.
 */
bool wl_hal_ob_program(const uint8_t *src, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* WL_FLASH_HAL_H */
