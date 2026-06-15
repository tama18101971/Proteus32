/*
 * aux_leds.h
 *
 * Created: 22.02.2020 17:42:44
 * Modified:15.02.2021
 *  Author: Tamagotchi
 */

#ifndef AUX_LEDS_H_
#define AUX_LEDS_H_

#include <stdbool.h>
#include "awu.h"
#include "button_gpio.h"
#include "config.h"

typedef enum {
	RED_LED = 0b001,
	GRN_LED	= 0b010,
	HBL_LED	= 0b100
} LED_Bits_t;

typedef enum {
	VWARNING_FAST =	0U,
	VWARNING_MEDIUM,
	VWARNING_SLOW,
	VWARNING_NONE
} WARNING_Fr_t;

typedef enum {
	IDM_OTHER = 0U,
	IDM_BEACON,
	IDM_VWARNING,
	IDM_STROBE
} IDM_Type_t;

typedef enum {
	PER_INIT = 0U,
	PER_FIRST,
	PER_SECOND,
	PER_END,
	PER_NEXT
} PER_Step_t;

typedef enum {
	AC_STOP = 0U,
	AC_REP_ATSTART,
	AC_REP_ATSTEPS,
	AC_RESERVED
} Array_Cmd_t;

union AUX_Cmd_u {
    struct __attribute__((packed)) {
        uint32_t device      : 3;  // биты 0..2   - чем управляем
        uint32_t cycle       : 1;  // бит 3       - зациклено (1) или нет (0)
        uint32_t count       : 4;  // биты 4..7   - кол-во повторов
        uint32_t duration0   : 9;  // биты 8..16  - длительность 1-го периода
        uint32_t duration1   : 9;  // биты 17..25 - длительность 2-го периода
        uint32_t brightness0 : 3;  // биты 26..28 - яркость 1-го периода
        uint32_t brightness1 : 3;  // биты 29..31 - яркость 2-го периода
    } aux;

    struct __attribute__((packed)) {
        uint32_t command  : 2;   // биты 0..1  - системная команда (0..3)
        uint32_t steps    : 3;   // биты 2..4  - шаг зацикливания (0..7)
        uint32_t is_cmd  : 27;  // биты 5..31 - флаг команды (если эти 27 бит == 0, то это системная команда)
    } cmd;

    uint32_t aux_cmd; // Полноценное 32-битное слово для Flash
};

typedef struct {
    uint32_t enable             : 1;  // бит 0      - активно или нет
    uint32_t period             : 3;  // биты 1..3  - 0: инит, 1: первый период, 2: второй период, 3: малый цикл завершен
    uint32_t current_brightness : 3;  // биты 4..6  - яркость для внутренних нужд
    uint32_t work_duration      : 9;  // биты 7..15 - длительность
    uint32_t idmode             : 2;  // биты 16..17- режим индикации, работающей в данный момент
    uint32_t arr_index          : 4;  // биты 18..21- индекс в массиве AUX команд
    uint32_t work_count         : 4;  // биты 22..25- количество повторов/оставшаяся работа
    uint32_t reserved           : 6;  // биты 26..31- резерв до полного 32-битного слова
} __attribute__((packed, aligned(4))) AUX_Ctr_t;

typedef enum {
	BCON_1S	= 0U,
	BCON_2S,
	BCON_3S,
	BCON_4S,
	BCON_5S,
	BCON_6S,
	BCON_7S,
	BCON_8S
} Beacon_Type_t;

typedef enum {
	BCK_TYPE_OFF = 0U,
	BCK_TYPE_ON
} BCK_Type_t;

typedef enum {
	BCK_COLOR_RED =	0U,
	BCK_COLOR_GRN
} BCK_Color_t;

extern union AUX_Cmd_u aux_cmd_red;
extern union AUX_Cmd_u aux_cmd_grn;
extern AUX_Ctr_t aux_ctl_red;
extern AUX_Ctr_t aux_ctl_grn;
extern union AUX_Cmd_u *p_red;
extern union AUX_Cmd_u *p_grn;

extern uint8_t vwarning;
extern uint8_t vwarning_old;

extern bool EnableArrayProcessingRed;
extern bool EnableArrayProcessingGrn;


bool is_aux_cmd(LED_Bits_t channel);
bool is_aux_beacon(LED_Bits_t channel);
bool is_aux_voltage(LED_Bits_t channel);
bool is_aux_strobe(LED_Bits_t channel);
bool is_aux_other(LED_Bits_t channel);
void stop_aux_beacon(LED_Bits_t channel);
void stop_aux_voltage(LED_Bits_t channel);
void stop_aux_strobe(LED_Bits_t channel);
void slot_batt_ind(void);
void start_aux_beacon(void);
void start_exaux_voltage(uint8_t beacon_mode);
void off_aux_cmd(LED_Bits_t channel, bool stop_lsi);
void set_aux_count(uint8_t count);
void set_aux_cmd(uint32_t command, awu_tick_t delay, bool enable, IDM_Type_t idm_mode);

void smart_off_leds(LED_Bits_t channel);

void aux_on(void);
void aux_off(void);

void test_aux_beacon(void);

void handle_aux(void);
void handle_aux_cmd(union AUX_Cmd_u *aux, AUX_Ctr_t *ubits);
union AUX_Cmd_u *handle_aux_array(const uint32_t pBuffer[], AUX_Ctr_t *ubits) ;

void aux_count(uint8_t count);

void aux_cmd_info_red(void);
void aux_cmd_info_grn(void);

#ifdef AUX_DEBUG
void debug_cmd(uint32_t cmd);
#endif


//#define SHORT_BLINK 		MAKE_CMD(RED_LED, 0, 1, 30, 60, 1, 0)
#define INFO_RED			MAKE_CMD(RED_LED, 0, 1, 30, 60, 1, 0)
#define INFO_GRN			MAKE_CMD(GRN_LED, 0, 1, 15, 75, 1, 0)
#define ERRO_RED			MAKE_CMD(RED_LED, 0, 3, 1, 6, 1, 0)

/**
 * @brief Формирует 32-битную AUX-команду из полей структуры AUX_Cmd_u
 *
 * @param device       канал управления (RED_LED=1, GRN_LED=2, HBL_LED=4)
 * @param cycle        зацикливание (0 — однократно, 1 — повторять)
 * @param count        количество повторов (0..15)
 * @param duration0    длительность 1-го полупериода в тиках AWU (0..511)
 * @param duration1    длительность 2-го полупериода в тиках AWU (0..511)
 * @param brightness0  яркость 1-го полупериода (0..7)
 * @param brightness1  яркость 2-го полупериода (0..7)
 *
 * @return 32-битная команда для set_aux_cmd() или массива
 */
#define MAKE_CMD(device, cycle, count, duration0, duration1, brightness0, brightness1) \
    ((uint32_t)((device) & 0x7)             | \
     ((uint32_t)((cycle) & 0x1) << 3)       | \
     ((uint32_t)((count) & 0xF) << 4)       | \
     ((uint32_t)((duration0) & 0x1FF) << 8) | \
     ((uint32_t)((duration1) & 0x1FF) << 17)| \
     ((uint32_t)((brightness0) & 0x7) << 26)| \
     ((uint32_t)((brightness1) & 0x7) << 29))

#endif /* AUX_LEDS_H_ */
