#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "wl_flash.h"

/* =========================================================================
 * UIF bit definitions (1 byte: bits 0..7)
 * ========================================================================= */
#define UIF_UI_INDEX_SHIFT    0u
#define UIF_UI_INDEX_MASK     (0x03u << UIF_UI_INDEX_SHIFT)
#define UIF_OFF_BCKL_SHIFT    2u
#define UIF_OFF_BCKL_MASK     (0x01u << UIF_OFF_BCKL_SHIFT)
#define UIF_USE_MEMORY_SHIFT  3u
#define UIF_USE_MEMORY_MASK   (0x01u << UIF_USE_MEMORY_SHIFT)
#define UIF_EAUX_ONOFF_SHIFT  4u
#define UIF_EAUX_ONOFF_MASK   (0x01u << UIF_EAUX_ONOFF_SHIFT)
#define UIF_UI2_SHIFT         5u
#define UIF_UI2_MASK          (0x07u << UIF_UI2_SHIFT)

_Static_assert((UIF_UI_INDEX_MASK & (UIF_OFF_BCKL_MASK | UIF_USE_MEMORY_MASK | UIF_EAUX_ONOFF_MASK | UIF_UI2_MASK)) == 0u &&
               (UIF_OFF_BCKL_MASK & (UIF_USE_MEMORY_MASK | UIF_EAUX_ONOFF_MASK | UIF_UI2_MASK)) == 0u &&
               (UIF_USE_MEMORY_MASK & (UIF_EAUX_ONOFF_MASK | UIF_UI2_MASK)) == 0u &&
               (UIF_EAUX_ONOFF_MASK & UIF_UI2_MASK) == 0u,
               "UIF fields overlap");
_Static_assert((UIF_UI_INDEX_MASK | UIF_OFF_BCKL_MASK | UIF_USE_MEMORY_MASK | UIF_EAUX_ONOFF_MASK | UIF_UI2_MASK) == 0xFFu,
               "UIF fields leave a gap");

/* =========================================================================
 * AUX bit definitions (1 byte: bits 0..7)
 * ========================================================================= */
#define AUXF_BCKLIGHT_TYPE_SHIFT   0u
#define AUXF_BCKLIGHT_TYPE_MASK    (0x01u << AUXF_BCKLIGHT_TYPE_SHIFT)
#define AUXF_BCKLIGHT_COLOR_SHIFT  1u
#define AUXF_BCKLIGHT_COLOR_MASK   (0x01u << AUXF_BCKLIGHT_COLOR_SHIFT)
#define AUXF_BEACON_ON_SHIFT       2u
#define AUXF_BEACON_ON_MASK        (0x01u << AUXF_BEACON_ON_SHIFT)
#define AUXF_BEACON_MODE_SHIFT     3u
#define AUXF_BEACON_MODE_MASK      (0x03u << AUXF_BEACON_MODE_SHIFT)
#define AUXF_BEACON_TYPE_SHIFT     5u
#define AUXF_BEACON_TYPE_MASK      (0x07u << AUXF_BEACON_TYPE_SHIFT)

_Static_assert((AUXF_BCKLIGHT_TYPE_MASK & (AUXF_BCKLIGHT_COLOR_MASK | AUXF_BEACON_ON_MASK | AUXF_BEACON_MODE_MASK | AUXF_BEACON_TYPE_MASK)) == 0u &&
               (AUXF_BCKLIGHT_COLOR_MASK & (AUXF_BEACON_ON_MASK | AUXF_BEACON_MODE_MASK | AUXF_BEACON_TYPE_MASK)) == 0u &&
               (AUXF_BEACON_ON_MASK & (AUXF_BEACON_MODE_MASK | AUXF_BEACON_TYPE_MASK)) == 0u &&
               (AUXF_BEACON_MODE_MASK & AUXF_BEACON_TYPE_MASK) == 0u,
               "AUXF fields overlap");
_Static_assert((AUXF_BCKLIGHT_TYPE_MASK | AUXF_BCKLIGHT_COLOR_MASK | AUXF_BEACON_ON_MASK | AUXF_BEACON_MODE_MASK | AUXF_BEACON_TYPE_MASK) == 0xFFu,
               "AUXF fields leave a gap");

/* =========================================================================
 * USR bit definitions (1 byte: bits 0..7)
 * ========================================================================= */
#define USRF_TCALIBR_SHIFT         0u
#define USRF_TCALIBR_MASK          (0x0Fu << USRF_TCALIBR_SHIFT)
#define USRF_ID_PLUS_SID_SHIFT     4u
#define USRF_ID_PLUS_SID_MASK      (0x01u << USRF_ID_PLUS_SID_SHIFT)
#define USRF_LOCK_MIN_EN_SHIFT     5u
#define USRF_LOCK_MIN_EN_MASK      (0x01u << USRF_LOCK_MIN_EN_SHIFT)
#define USRF_LOCK_MIN_TIME_SHIFT   6u
#define USRF_LOCK_MIN_TIME_MASK    (0x03u << USRF_LOCK_MIN_TIME_SHIFT)

_Static_assert((USRF_TCALIBR_MASK & (USRF_ID_PLUS_SID_MASK | USRF_LOCK_MIN_EN_MASK | USRF_LOCK_MIN_TIME_MASK)) == 0u &&
               (USRF_ID_PLUS_SID_MASK & (USRF_LOCK_MIN_EN_MASK | USRF_LOCK_MIN_TIME_MASK)) == 0u &&
               (USRF_LOCK_MIN_EN_MASK & USRF_LOCK_MIN_TIME_MASK) == 0u,
               "USRF fields overlap");
_Static_assert((USRF_TCALIBR_MASK | USRF_ID_PLUS_SID_MASK | USRF_LOCK_MIN_EN_MASK | USRF_LOCK_MIN_TIME_MASK) == 0xFFu,
               "USRF fields leave a gap");

/* Field extraction and modification macros */
#define CFG_GET_FIELD(byte, mask, shift) \
    ((uint8_t)(((uint8_t)(byte) & (uint8_t)(mask)) >> (shift)))

#define CFG_SET_FIELD(byte, mask, shift, val) \
    ((uint8_t)(((uint8_t)(byte) & ~(uint8_t)(mask)) | (((uint8_t)(val) << (shift)) & (uint8_t)(mask))))

/* Encode/decode round-trip verification */
_Static_assert(CFG_GET_FIELD(CFG_SET_FIELD(0, UIF_UI_INDEX_MASK, UIF_UI_INDEX_SHIFT, 3), UIF_UI_INDEX_MASK, UIF_UI_INDEX_SHIFT) == 3, "UIF ui_index round-trip");
_Static_assert(CFG_GET_FIELD(CFG_SET_FIELD(0, AUXF_BEACON_MODE_MASK, AUXF_BEACON_MODE_SHIFT, 3), AUXF_BEACON_MODE_MASK, AUXF_BEACON_MODE_SHIFT) == 3, "AUXF beacon_mode round-trip");
_Static_assert(CFG_GET_FIELD(CFG_SET_FIELD(0, AUXF_BEACON_TYPE_MASK, AUXF_BEACON_TYPE_SHIFT, 7), AUXF_BEACON_TYPE_MASK, AUXF_BEACON_TYPE_SHIFT) == 7, "AUXF beacon_type round-trip");
_Static_assert(CFG_GET_FIELD(CFG_SET_FIELD(0, USRF_TCALIBR_MASK, USRF_TCALIBR_SHIFT, 15), USRF_TCALIBR_MASK, USRF_TCALIBR_SHIFT) == 15, "USRF t_calibr round-trip");
_Static_assert(CFG_GET_FIELD(CFG_SET_FIELD(0, USRF_LOCK_MIN_TIME_MASK, USRF_LOCK_MIN_TIME_SHIFT, 3), USRF_LOCK_MIN_TIME_MASK, USRF_LOCK_MIN_TIME_SHIFT) == 3, "USRF lock_min_time round-trip");
_Static_assert(CFG_GET_FIELD(CFG_SET_FIELD(0, UIF_UI2_MASK, UIF_UI2_SHIFT, 7), UIF_UI2_MASK, UIF_UI2_SHIFT) == 7, "UIF ui2 round-trip");
_Static_assert(((uint16_t)((uint8_t)(510u >> 1) << 1)) == 510u, "ui1 step>>1 quantisation round-trip");

/* Field Accessors */
/* default UI1: 9-битный шаг 0..511 хранится как step>>1 (8 бит).
 * Потеря одного шага (<=0.2% яркости) окупает запись 16 Б. */
#define CFG_GET_UI1(cfg)              ((uint16_t)((uint16_t)((cfg).ui1) << 1))
#define CFG_SET_UI1(cfg, val)         ((cfg).ui1 = (uint8_t)((uint16_t)(val) >> 1))
#define UIF_GET_UI_INDEX(uif)         CFG_GET_FIELD(uif, UIF_UI_INDEX_MASK, UIF_UI_INDEX_SHIFT)
#define UIF_GET_OFF_BCKL(uif)         CFG_GET_FIELD(uif, UIF_OFF_BCKL_MASK, UIF_OFF_BCKL_SHIFT)
#define UIF_GET_USE_MEMORY(uif)       CFG_GET_FIELD(uif, UIF_USE_MEMORY_MASK, UIF_USE_MEMORY_SHIFT)
#define UIF_GET_EAUX_ONOFF(uif)       CFG_GET_FIELD(uif, UIF_EAUX_ONOFF_MASK, UIF_EAUX_ONOFF_SHIFT)

#define UIF_SET_UI_INDEX(uif, val)    ((uif) = CFG_SET_FIELD(uif, UIF_UI_INDEX_MASK, UIF_UI_INDEX_SHIFT, val))
#define UIF_SET_OFF_BCKL(uif, val)    ((uif) = CFG_SET_FIELD(uif, UIF_OFF_BCKL_MASK, UIF_OFF_BCKL_SHIFT, val))
#define UIF_SET_USE_MEMORY(uif, val)  ((uif) = CFG_SET_FIELD(uif, UIF_USE_MEMORY_MASK, UIF_USE_MEMORY_SHIFT, val))
#define UIF_SET_EAUX_ONOFF(uif, val)  ((uif) = CFG_SET_FIELD(uif, UIF_EAUX_ONOFF_MASK, UIF_EAUX_ONOFF_SHIFT, val))

#define UIF_GET_UI2(uif)              CFG_GET_FIELD(uif, UIF_UI2_MASK, UIF_UI2_SHIFT)
#define UIF_SET_UI2(uif, val)         ((uif) = CFG_SET_FIELD(uif, UIF_UI2_MASK, UIF_UI2_SHIFT, val))

#define AUXF_GET_BCKLIGHT_TYPE(aux)   CFG_GET_FIELD(aux, AUXF_BCKLIGHT_TYPE_MASK, AUXF_BCKLIGHT_TYPE_SHIFT)
#define AUXF_GET_BCKLIGHT_COLOR(aux)  CFG_GET_FIELD(aux, AUXF_BCKLIGHT_COLOR_MASK, AUXF_BCKLIGHT_COLOR_SHIFT)
#define AUXF_GET_BEACON_ON(aux)       CFG_GET_FIELD(aux, AUXF_BEACON_ON_MASK, AUXF_BEACON_ON_SHIFT)
#define AUXF_GET_BEACON_MODE(aux)     CFG_GET_FIELD(aux, AUXF_BEACON_MODE_MASK, AUXF_BEACON_MODE_SHIFT)
#define AUXF_GET_BEACON_TYPE(aux)     CFG_GET_FIELD(aux, AUXF_BEACON_TYPE_MASK, AUXF_BEACON_TYPE_SHIFT)

#define AUXF_SET_BCKLIGHT_TYPE(aux, val)  ((aux) = CFG_SET_FIELD(aux, AUXF_BCKLIGHT_TYPE_MASK, AUXF_BCKLIGHT_TYPE_SHIFT, val))
#define AUXF_SET_BCKLIGHT_COLOR(aux, val) ((aux) = CFG_SET_FIELD(aux, AUXF_BCKLIGHT_COLOR_MASK, AUXF_BCKLIGHT_COLOR_SHIFT, val))
#define AUXF_SET_BEACON_ON(aux, val)      ((aux) = CFG_SET_FIELD(aux, AUXF_BEACON_ON_MASK, AUXF_BEACON_ON_SHIFT, val))
#define AUXF_SET_BEACON_MODE(aux, val)    ((aux) = CFG_SET_FIELD(aux, AUXF_BEACON_MODE_MASK, AUXF_BEACON_MODE_SHIFT, val))
#define AUXF_SET_BEACON_TYPE(aux, val)    ((aux) = CFG_SET_FIELD(aux, AUXF_BEACON_TYPE_MASK, AUXF_BEACON_TYPE_SHIFT, val))

#define USRF_GET_TCALIBR(usr)         CFG_GET_FIELD(usr, USRF_TCALIBR_MASK, USRF_TCALIBR_SHIFT)
#define USRF_GET_ID_PLUS_SID(usr)     CFG_GET_FIELD(usr, USRF_ID_PLUS_SID_MASK, USRF_ID_PLUS_SID_SHIFT)
#define USRF_GET_LOCK_MIN_EN(usr)     CFG_GET_FIELD(usr, USRF_LOCK_MIN_EN_MASK, USRF_LOCK_MIN_EN_SHIFT)
#define USRF_GET_LOCK_MIN_TIME(usr)   CFG_GET_FIELD(usr, USRF_LOCK_MIN_TIME_MASK, USRF_LOCK_MIN_TIME_SHIFT)

#define USRF_SET_TCALIBR(usr, val)        ((usr) = CFG_SET_FIELD(usr, USRF_TCALIBR_MASK, USRF_TCALIBR_SHIFT, val))
#define USRF_SET_ID_PLUS_SID(usr, val)    ((usr) = CFG_SET_FIELD(usr, USRF_ID_PLUS_SID_MASK, USRF_ID_PLUS_SID_SHIFT, val))
#define USRF_SET_LOCK_MIN_EN(usr, val)    ((usr) = CFG_SET_FIELD(usr, USRF_LOCK_MIN_EN_MASK, USRF_LOCK_MIN_EN_SHIFT, val))
#define USRF_SET_LOCK_MIN_TIME(usr, val)  ((usr) = CFG_SET_FIELD(usr, USRF_LOCK_MIN_TIME_MASK, USRF_LOCK_MIN_TIME_SHIFT, val))

#define SLOTS_COUNT 10

/*
 * Полезный груз записи WL_FLASH (9 байт при WL_RECORD_SIZE=16 и CRC-16).
 * Служебный заголовок записи — seq(4), version(1), crc(2) — хранит сама библиотека.
 *
 * Раскладка payload (ровно 9 байт):
 *   байт 0      : ui1 — default UI1 step >> 1 (0..255), восстанавливается << 1
 *   байт 1      : uif — биты 0..4 UIF-флаги, биты 5..7 default_ui2 (CMODE_*)
 *   байт 2      : aux (uint8_t)
 *   байт 3      : usr (uint8_t)
 *   байты 4..8  : slots_packed[5] (10 слотов по 4 бита)
 */
typedef struct __attribute__((packed)) {
    uint8_t  ui1;             /* default UI1 step >> 1 (0..255) */
    uint8_t  uif;             /* UIF bits 0..4 + default_ui2 bits 5..7 */
    uint8_t  aux;
    uint8_t  usr;
    uint8_t  slots_packed[5];
} app_cfg_t;

_Static_assert(sizeof(app_cfg_t) == 9, "app_cfg_t size must be exactly 9 bytes");
WL_ASSERT_PAYLOAD_FITS(app_cfg_t);

#define EE  (*WL_DATA_AS(app_cfg_t))

/**
 * @brief Чтение элемента magic-слота по индексу (0..SLOTS_COUNT-1).
 *
 * Слоты упакованы по 4 бита: слот 2j в младшем ниббле, слот 2j+1 в старшем.
 */
static inline uint8_t slot_get(uint8_t index) {
    if (index >= SLOTS_COUNT)
        return 0;
    uint8_t b = EE.slots_packed[index >> 1];
    return (index & 1u) ? (b >> 4) : (b & 0x0Fu);
}

/**
 * @brief Запись элемента magic-слота по индексу (0..SLOTS_COUNT-1).
 *
 * Упаковывает 4-битное значение в slots_packed[5] и помечает WL грязным при изменении.
 */
static inline void slot_set(uint8_t index, uint8_t value) {
    if (index >= SLOTS_COUNT)
        return;
    uint8_t j = index >> 1;
    uint8_t old_b = EE.slots_packed[j];
    value &= 0x0Fu;
    uint8_t new_b;
    if (index & 1u) {
        new_b = (old_b & 0x0Fu) | (uint8_t)(value << 4);
    } else {
        new_b = (old_b & 0xF0u) | value;
    }
    if (new_b != old_b) {
        EE.slots_packed[j] = new_b;
        //wl_mark_dirty();
    }
}
