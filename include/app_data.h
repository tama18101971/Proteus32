#pragma once

#include <stdint.h>
#include "wl_flash.h"

struct __attribute__((packed)) aux_flags {
    uint8_t bcklight_type  : 1;
    uint8_t bcklight_color : 1;
    uint8_t beacon_on      : 1;
    uint8_t beacon_mode    : 2;
    uint8_t beacon_type    : 3;
};

struct __attribute__((packed)) usr_flags {
    uint8_t t_calibr       : 4;
    uint8_t id_plus_sid    : 1;
    uint8_t lock_min_en    : 1;
    uint8_t lock_min_time  : 2;
};

struct __attribute__((packed)) ui_flags {
    uint8_t ui_index       : 2;
    uint8_t off_bckl       : 1;
    uint8_t use_memory     : 1;
    uint8_t eaux_onoff     : 1;
    uint8_t reserved       : 3;
};

/*
 * Полезный груз записи WL_FLASH (WL_DATA_SIZE байт при WL_RECORD_SIZE=16 и
 * CRC-32). Служебный заголовок записи — seq(4), version(1), crc(4) — хранит
 * сама библиотека, дублировать его здесь не нужно.
 *
 * packed обязателен: uint16_t в начале иначе округляет sizeof до 8 из-за
 * выравнивания, а груз должен уложиться ровно в WL_DATA_SIZE (7 байт).
 * Побайтовый доступ к default_ui1 при этом не хуже прежнего — в union ee_view
 * v1.x поле лежало по нечётному смещению 5.
 */
typedef struct __attribute__((packed)) {
    uint16_t         default_ui1;
    struct ui_flags  uif;
    struct aux_flags aux;
    struct usr_flags usr;
    uint8_t          default_ui2;
    uint8_t          default_ui3;
} app_cfg_t;

WL_ASSERT_PAYLOAD_FITS(app_cfg_t);

#define EE  (*WL_DATA_AS(app_cfg_t))
