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
    uint8_t eaux_onoff     : 1;
    uint8_t id_plus_sid    : 1;
    uint8_t reserved       : 2;
};

struct __attribute__((packed)) ui_flags {
    uint8_t ui_index       : 2;
    uint8_t off_bckl       : 1;
    uint8_t use_memory     : 1;
    uint8_t reserved       : 4;
};

union ee_view {
    struct ee_data raw;
    struct __attribute__((packed)) {
        uint32_t seq;
        uint8_t  version;
        uint16_t default__ui1;
        struct ui_flags  uif;
        struct aux_flags aux;
        struct usr_flags usr;
        uint8_t default__ui2;
        uint8_t default__ui3;
        ee_crc_t crc;
    };
};

_Static_assert(sizeof(union ee_view) == EE_STRUCT_SIZE,
               "ee_view overlay size mismatch");

#define EE  (*(union ee_view *)&ee)
