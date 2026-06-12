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
#include "button.h"

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
	struct {
		unsigned int device :3;			// биты 0-2 - определяют, чем управляем.
		unsigned int cycle :1;			// бит 3	- зациклено или нет
		unsigned int count :4;			// биты 4-7	- к-во повторов при зацикливании
		unsigned int duration0 :9;		// 8-16  байт длительности в 1/60. Первые 9 байт для первого периода, вторые 9 байт для второго
		unsigned int duration1 :9;		// 17-25 байт длительности в 1/60. Первые 9 байт для первого периода, вторые 9 байт для второго
		unsigned int brightness0 :3;	// 26-28 яркость первого периода. In MODES.
		unsigned int brightness1 :3;	// 29-31 яркость второго периода. In MODES.
	} aux;
	struct {
		unsigned int command :2;		// команда 0 - стоп, 1 - вечный цикл с начала, 2 - вечный цикл с back_steps шагов назад
		unsigned int steps : 3;			// на какую от начала позицию зацикливаться
		unsigned int is_cmd : 27;		// если эти 24 бита = 0 то это комманда обработчику массива, если нет - обработчику AUX
	} cmd;
	uint32_t aux_cmd;
};

typedef struct {
	unsigned int enable :1;				// бит 0		- активно или нет
	unsigned int period :3;				// бит 1..3		- 0 - инициализация, 1 - первый период, 2 - второй период, 3 - малый цикл завершен
	unsigned int current_brightness: 3;	// биты 4..6 	- яркость для внутренних нужд
	unsigned int work_duration :9;		// биты 7..15 	- длительность
	unsigned int idmode :2;				// биты 16..17	- режим индикации, работающей в данный момент
	unsigned int arr_index :4;			// биты 18..21  - индекс в массиве AUX команд
	unsigned int work_count : 4;
	unsigned int reserved :6;
} AUX_Ctr_t;

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


bool is_aux_cmd(void);
bool is_aux_beacon(void);
bool is_aux_voltage(void);
void stop_aux_beacon(void);
void stop_aux_voltage(void);
void stop_aux_strobe(void);
void slot_batt_ind(void);
void start_aux_beacon(void);
void start_exaux_voltage(uint8_t beacon_mode);
void off_aux_cmd(bool stop_wdt);
void set_aux_count(uint8_t count);
void set_aux_cmd(uint32_t command, awu_tick_t delay, bool enable, IDM_Type_t idm_mode);

void smart_off_red(void);
void smart_off_green(void);

void aux_on(void);
void aux_off(void);

void test_aux_beacon(void);

void handle_aux(void);
void handle_aux_cmd(union AUX_Cmd_u *aux, AUX_Ctr_t *ubits);
union AUX_Cmd_u *handle_aux_array(const uint32_t pBuffer[], AUX_Ctr_t *ubits) ;

void aux_count(uint8_t count);

void aux_cmd_info_red(void);
void aux_cmd_info_grn(void);

#define SHORT_BLINK 		0b00000000000000010000000000010001
#define INFO_RED			0b00000000000000010001010100010001
#define INFO_GRN			0b00000000000000010100010000100010

#endif /* AUX_LEDS_H_ */
