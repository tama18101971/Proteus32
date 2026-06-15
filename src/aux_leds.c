/*
 * aux_leds.c
 *
 * Created: 22.02.2020 17:42:26
 * Modified:15.02.2021
 *  Author: Tamagotchi
 */


#include "aux_leds.h"
//#include "eeprom.h"
//#include "soft_start.h"
#include "main.h"
#include "button_gpio.h"

#include "led_gpio.h"
//#include "macros.h"
//#include "adc.h"
#include "wl_flash.h"


extern struct ee_data ee;

uint8_t vwarning = VWARNING_NONE;
uint8_t vwarning_old = VWARNING_NONE;

const uint32_t buf_red[5]		= {0b101000000000000010001010001, 0b100010100000000010001010001, 0b101000000000000010001000001, 0b100010100000000010001010001, 0x6};
const uint32_t buf_grn[3] 		= {0b101000000000000010001011010, 0b100010100000000010001011010, 0};
//const uint32_t buf_sos_red[5]	= {0xA0571, 0x4140A31, 0x4141E31, 0x4140A31, 0x1}; // .=10 -=30
const uint32_t buf_sos_red[5]	= {0b10100000010101110001, 0b0100000101000000101000110001, 0b0100000101000001111000110001, 0b0100000101000000101000110001, 1};

union AUX_Cmd_u *p_red;
union AUX_Cmd_u *p_grn;
union AUX_Cmd_u *p_hbl;

union AUX_Cmd_u aux_cmd_red;
union AUX_Cmd_u aux_cmd_grn;
union AUX_Cmd_u aux_cmd_hbl;

AUX_Ctr_t aux_ctl_red;
AUX_Ctr_t aux_ctl_grn;
AUX_Ctr_t aux_ctl_hbl;

bool EnableArrayProcessingRed = false;
bool EnableArrayProcessingGrn = false;

const uint32_t *current_buf_red = NULL;
const uint32_t *current_buf_grn = NULL;

static AUX_Ctr_t *const aux_ctl_table[] = {
	[RED_LED] = &aux_ctl_red,
	[GRN_LED] = &aux_ctl_grn,
	[HBL_LED] = &aux_ctl_hbl,
};

static union AUX_Cmd_u *const aux_cmd_table[] = {
	[RED_LED] = &aux_cmd_red,
	[GRN_LED] = &aux_cmd_grn,
	[HBL_LED] = &aux_cmd_hbl,
};

static union AUX_Cmd_u **const aux_ptr_table[] = {
	[RED_LED] = &p_red,
	[GRN_LED] = &p_grn,
	[HBL_LED] = &p_hbl,
};

void start_aux_array(const uint32_t *pArray) {
	LED_Bits_t channel = (LED_Bits_t)(pArray[0] & 0x07);

	if (channel < RED_LED || channel > GRN_LED) return;

	AUX_Ctr_t *ctl = aux_ctl_table[channel];
	bool *flag = (channel == RED_LED) ? &EnableArrayProcessingRed : &EnableArrayProcessingGrn;

	uint32_t *p = (uint32_t*)ctl;
	*p = 0;

	*flag = true;

	if (channel == RED_LED) {
		p_red = (union AUX_Cmd_u*)pArray;
		current_buf_red = pArray;
	} else {
		p_grn = (union AUX_Cmd_u*)pArray;
		current_buf_grn = pArray;
	}

	ctl->enable = true;
}

/************************************************************************/
/*                                                                      */
/************************************************************************/
void smart_off_leds(LED_Bits_t channel) {
	if (ee.aux.bcklight_type != BCK_TYPE_ON) {
		if (channel == RED_LED) off_red();
		else off_grn();
		return;
	}

	switch (channel)
	{
	case RED_LED:
		(ee.aux.bcklight_color == BCK_COLOR_RED) ? on_red_l() : off_red();
		break;
	case GRN_LED:
		(ee.aux.bcklight_color == BCK_COLOR_GRN) ? on_grn_l() : off_grn();
		break;
	default:
		break;
	}
}

static AUX_Ctr_t* get_aux_ctl(LED_Bits_t channel) {
	if (channel < RED_LED || channel > HBL_LED) return NULL;
	return aux_ctl_table[channel];
}

bool is_aux_mode(LED_Bits_t channel, IDM_Type_t mode) {
	AUX_Ctr_t *ctl = get_aux_ctl(channel);
	return (ctl && ctl->enable && (ctl->idmode == mode));
}

bool is_aux_active(LED_Bits_t channel) {
	AUX_Ctr_t *ctl = get_aux_ctl(channel);
	return (ctl && ctl->enable);
}

bool is_aux_cmd(LED_Bits_t channel) { return is_aux_active(channel); }
bool is_aux_beacon(LED_Bits_t channel) { return is_aux_mode(channel, IDM_BEACON); }
bool is_aux_voltage(LED_Bits_t channel) { return is_aux_mode(channel, IDM_VWARNING); }
bool is_aux_strobe(LED_Bits_t channel) { return is_aux_mode(channel, IDM_STROBE); }
bool is_aux_other(LED_Bits_t channel) { return is_aux_mode(channel, IDM_OTHER); }

/************************************************************************/
/*                                                                      */
/************************************************************************/
static union AUX_Cmd_u* get_aux_cmd(LED_Bits_t channel) {
	if (channel < RED_LED || channel > HBL_LED) return NULL;
	return aux_cmd_table[channel];
}

static union AUX_Cmd_u** get_aux_ptr(LED_Bits_t channel) {
	if (channel < RED_LED || channel > HBL_LED) return NULL;
	return aux_ptr_table[channel];
}

void off_aux_cmd(LED_Bits_t channel, bool stop_lsi) {
	AUX_Ctr_t *ctl = get_aux_ctl(channel);
	if (!ctl) return;

	ctl->enable = false;
	ctl->period = PER_INIT;
	ctl->idmode = IDM_OTHER;

	smart_off_leds(channel);

	if (stop_lsi) {
		if (!is_aux_active(RED_LED) && !is_aux_active(GRN_LED) && !is_aux_active(HBL_LED)) {
			awu_stop(true);
		}
	}
}

#ifdef USE_BATTIND
/************************************************************************/
/*                                                                      */
/************************************************************************/

void set_aux_count(uint8_t count) {
	stop_wakeup();																		// для уверенности

	as_red.acmd.aux.brightness0 = 1;
	as_red.acmd.aux.brightness1 = 0;
	as_red.acmd.aux.duration0 = AUX_500MS;
	as_red.acmd.aux.duration1 = AUX_60MS;
	as_red.acmd.aux.count = count;
	as_red.acmd.aux.cycle = 0;

#ifdef USE_EX_AUX
	if (count == 1)
		as_red.acmd.aux.device = RED_BIT;
	else if (count == 2)
		as_red.acmd.aux.device = RED_BIT | GRN_BIT;
	else
		as_red.acmd.aux.device = GRN_BIT;
#else
	if(count < 0) as_red.acmd.aux.device = RED_BIT;
	else as_red.acmd.aux.device = GRN_BIT;
	#endif

	if (ee.usr.id_plus_sid) {													// флаг включения дублирования ИДа СИДом
		as_red.acmd.aux.brightness0 |= step_user ? step_user : step_default;//	если дублируем - используем текущий step и сохраняем младший бит для работы ИДа
		as_red.acmd.aux.device |= HBL_BIT;
	}

	as_red.ubits.enable = true;
	AUX_Set_Delay(AUX_15MS);								//включаем сторожевой таймер интервалом delay (WDTO_15MS)
}
#endif

void aux_cmd_info_red(void) {
	start_aux_cmd(INFO_RED, AUX_60MS, true, IDM_OTHER);
}

void aux_cmd_info_grn(void) {
	start_aux_cmd(INFO_GRN, AUX_60MS, true, IDM_OTHER);
}

/**
 * @brief Установка AUX-команды для канала, определённого битами 0..2 параметра command
 * @param command   32-битная команда: биты 0..2 — канал (RED/GRN/HBL), остальные — параметры
 * @param delay     задержка перед выполнением (таймер AWU)
 * @param enable    true — запустить выполнение, false — команда не активна
 * @param idm_mode  режим индикации (beacon, vwarning, strobe, other)
 */
void start_aux_cmd(uint32_t command, awu_tick_t delay, bool enable, IDM_Type_t idm_mode) {
    LED_Bits_t channel = command & 0x07;

	#ifdef AUX_DEBUG
	debug_cmd(command);
	#endif

    union AUX_Cmd_u *cmd = get_aux_cmd(channel);
    AUX_Ctr_t *ctl = get_aux_ctl(channel);
    union AUX_Cmd_u **ptr = get_aux_ptr(channel);

    if (!cmd || !ctl || !ptr) return;

    if (channel == RED_LED) EnableArrayProcessingRed = false;
    else if (channel == GRN_LED) EnableArrayProcessingGrn = false;

    cmd->aux_cmd = command;

    ctl->enable = enable;
    ctl->idmode = idm_mode;
    ctl->period = PER_INIT;
    ctl->current_brightness = 0;
    ctl->work_duration = 0;
    ctl->work_count = 0;

    *ptr = cmd;

    if (delay != AUX_15MS) {
        awu_set_delay(delay);
    }
}

static void SetBrightness(LED_Bits_t device, uint8_t brightness) {
	switch (device) {
		case RED_LED:
			if (brightness) on_red_h();
			else smart_off_leds(RED_LED);
			break;
		case GRN_LED:
			if (brightness) on_grn_h();
			else smart_off_leds(GRN_LED);
			break;
		case HBL_LED:
			// TODO: Сделать
			break;
		default:
			break;
	}
}

//***********************************************[24/02/2020 Tamagotchi]************************************************************//
//
//  24-31 биты	- step 2. Яркость в шагах, если основной диод, для ИДов 0- выкл, 1- полная яркость, 2- вход с подтяжкой.
//	16-23 биты	- step 1. Яркость в шигах, если основной диод, для ИДов 0- выкл, 1- полная яркость, 2- вход с подтяжкой.
//
//  12-15 биты	- длительность в WDT0 2.
//	8-11  биты	- длительность в WDT0 1.
//
//  4-7 бит		- к-во повторов при зацикливании, если все 0 - завершить цикл. Не использует 3-й бит. Даже если 3-й бит не установлен зацикливаем до 0.
//
//  3 бит		- переход на начало (вечное зацикливание).
//  0-2			- биты определяющие, чем управляем.
//
//********************************************************************************************************************************//

static bool handle_half_period(union AUX_Cmd_u *aux, AUX_Ctr_t *ubits,
								uint8_t brightness, uint16_t next_duration, PER_Step_t next_period) {
	LED_Bits_t device = (LED_Bits_t) aux->aux.device;

	if (brightness != ubits->current_brightness) {
		SetBrightness(device, brightness);
		ubits->current_brightness = brightness;
	}

	uint16_t duration = (uint16_t)(ubits->work_duration);

	if (--duration) {
		ubits->work_duration = duration;
		return false;
	}

	ubits->period = next_period;
	ubits->work_duration = next_duration;
	return true;
}

void handle_aux_cmd(union AUX_Cmd_u *aux, AUX_Ctr_t *ubits) {

	if (ubits->enable) {

		if (ubits->period == PER_INIT) {
			ubits->work_duration = aux->aux.duration0;
			ubits->work_count = aux->aux.count;
			ubits->period = PER_FIRST;
		}

		if (ubits->period == PER_FIRST) {
			handle_half_period(aux, ubits, aux->aux.brightness0, aux->aux.duration1, PER_SECOND);
		}

		else if (ubits->period == PER_SECOND) {
			handle_half_period(aux, ubits, aux->aux.brightness1, 0, PER_END);
		}

		else if (ubits->period == PER_END) {

			ubits->work_duration = aux->aux.duration0;
			ubits->period = PER_FIRST;

			uint8_t cycle = (uint8_t) aux->aux.cycle;
			uint8_t counter = (uint8_t) ubits->work_count;

			if (!cycle) {
				counter--;
				ubits->work_count = counter;

				if (!counter) {
					ubits->enable = false;
					ubits->period = PER_NEXT;
				}
			}
		}
	}
}

union AUX_Cmd_u *handle_aux_array(const uint32_t pArray[], AUX_Ctr_t *ctr) {

	union AUX_Cmd_u *p = (union AUX_Cmd_u*) pArray;

	p += ctr->arr_index;

	if (p->cmd.is_cmd == 0) {					// если 0 это команда для обработчика массива, а не моргалки

		switch ((Array_Cmd_t) p->cmd.command) {
		case AC_STOP:							// остановить выполнение комманд
			p = NULL;
			break;
		case AC_REP_ATSTART:					// повторять в цикле с начала массива
			ctr->arr_index = 0;					// идем на начало массива
			p = (union AUX_Cmd_u*) pArray;		// указатель тоже на начало
			ctr->period = PER_INIT;				// Период в инициализацию
			ctr->enable = true;					// Включаем AUX
			break;
		case AC_REP_ATSTEPS:					// повторить в цикле не с начала массива
			ctr->arr_index = p->cmd.steps;		// идем на начало массива и отступаем
			p = (union AUX_Cmd_u*) pArray;		// указатель тоже на начало
			p += ctr->arr_index;
			ctr->period = PER_INIT;				// Период в инициализацию
			ctr->enable = true;					// Включаем AUX
			break;
		default:
			break;
		}
	} else {									// если это не команда для обработки массива - это комманда для отмаргивателя.
		if (ctr->period == PER_NEXT) {			// если NEXT значит требуется переход на следующий элмент массива.
			ctr->period = PER_INIT;				// Период в инициализацию
			ctr->arr_index++;					// увеличиваем индекс в массиве
			p++;								// сдвигаем на новую команду
			ctr->enable = true;					// Включаем AUX
		}
	}
	return p;									// возвращаем указатель на union AUX_Cmd_u
}

void handle_aux(void) {
	if (EnableArrayProcessingRed) {
		p_red = handle_aux_array(current_buf_red, &aux_ctl_red);
		if (p_red == NULL) { EnableArrayProcessingRed = false; }
		else { handle_aux_cmd(p_red, &aux_ctl_red); }
	} else if (p_red) {
		handle_aux_cmd(p_red, &aux_ctl_red);
	}

	if (EnableArrayProcessingGrn) {
		p_grn = handle_aux_array(current_buf_grn, &aux_ctl_grn);
		if (p_grn == NULL) { EnableArrayProcessingGrn = false; }
		else { handle_aux_cmd(p_grn, &aux_ctl_grn); }
	} else if (p_grn) {
		handle_aux_cmd(p_grn, &aux_ctl_grn);
	}
}

/************************************************************************/
/*                                                                      */
/************************************************************************/
void start_aux_beacon(void) {
	uint32_t cmd = 0;
	uint8_t beacon_type = ee.aux.beacon_type;					// чем моргаем
	uint16_t beacon_mode = (uint16_t) ee.aux.beacon_mode;		// частота моргания

	cmd = (0b00000000000000010000000100001000 | beacon_type | ((beacon_mode + 7) << 12));
	start_aux_cmd(cmd, AUX_1S, true, IDM_BEACON);
}

#ifdef USE_EX_AUX
/************************************************************************/
/*                   �������� ����������� ������������                  */
/************************************************************************/
void start_exaux_voltage(WARNING_Fr_t ex_mode) {
	uint16_t low16 = 0;
	switch (ex_mode) {
	case VWARNING_FAST:
		low16 = 0b0111000000001001;	// 2sec
		break;
	case VWARNING_MEDIUM:
		low16 = 0b1000000000001011;	// 4sec
		break;
	case VWARNING_SLOW:
		low16 = 0b1001000000001010;	// 8sec
		break;
	}
	start_aux_cmd((0b00000000000000010000000000000000 | low16), AUX_1S, true, IDM_VWARNING);
}
#endif

/************************************************************************/
/*                                                                      */
/************************************************************************/
void stop_aux_mode(LED_Bits_t channel, IDM_Type_t mode) {
	if (is_aux_mode(channel, mode))
		off_aux_cmd(channel, false);
}

void stop_aux_beacon(LED_Bits_t channel) { stop_aux_mode(channel, IDM_BEACON); }
void stop_aux_voltage(LED_Bits_t channel) { stop_aux_mode(channel, IDM_VWARNING); }
void stop_aux_strobe(LED_Bits_t channel) { stop_aux_mode(channel, IDM_STROBE); }

/************************************************************************/
/*                                                                      */
/************************************************************************/
void slot_batt_ind(void) /* в массив настраиваемых слотов */
{
#ifdef USE_BATTIND
	stop_aux_beacon(RED_LED);					// 20.03.2021
	stop_aux_beacon(GRN_LED);					// 15.06.2026
	set_aux_count(bat_get_flashes());
#endif
}

void aux_on(void) {
	start_aux_cmd(INFO_GRN, AUX_60MS, true, IDM_OTHER);
}

void aux_off(void) {
	start_aux_cmd(INFO_RED, AUX_60MS, true, IDM_OTHER);
}

void aux_count(uint8_t count) {
	uint32_t cmd;
	if (count)
		cmd = 0b00000000000000010100000100000010 | (count << 4);	// зеленые, если не ноль
	else
		cmd = 0b00000000000000010010001000010001;					// одна красная, если 0
	start_aux_cmd(cmd, AUX_250MS, true, IDM_OTHER);
}

/**********************************************************************************************/
#ifdef AUX_DEBUG
// Функция для отладки: вывод полей команды
void debug_cmd(uint32_t cmd) {
    union AUX_Cmd_u u;
    u.aux_cmd = cmd;

    EE_PRINTF("[AUX] CMD: 0x%08lX\n", cmd);
    EE_PRINTF("  device    = %d\n", u.aux.device);
    EE_PRINTF("  cycle     = %d\n", u.aux.cycle);
    EE_PRINTF("  count     = %d\n", u.aux.count);
    EE_PRINTF("  duration0 = %d (%d ms)\n", u.aux.duration0, u.aux.duration0 * 15);
    EE_PRINTF("  duration1 = %d (%d ms)\n", u.aux.duration1, u.aux.duration1 * 15);
    EE_PRINTF("  bright0   = %d\n", u.aux.brightness0);
    EE_PRINTF("  bright1   = %d\n", u.aux.brightness1);
}
#endif /* AUX_DEBUG */

/************************************************************************/
/*                                                                      */
/************************************************************************/
void test_aux_beacon(void) {
	uint32_t *p = (uint32_t*)&aux_ctl_red;
	*p = 0;
	p = (uint32_t*)&aux_ctl_grn;
	*p = 0;

	aux_ctl_red.idmode			= IDM_BEACON;

	aux_ctl_grn.idmode			= IDM_BEACON;
	aux_ctl_grn.arr_index		= 0;
	aux_ctl_grn.enable			= true;
}
