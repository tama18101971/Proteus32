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

const uint32_t strobes[3][5]	= {	{0xA0571, 0x4140A31, 0x4141E31, 0x4140A31, 0x1 }, // .=10 -=30 SOS
									{},
									{},
									};

union AUX_Cmd_u *p_red;
union AUX_Cmd_u *p_grn;

union AUX_Cmd_u aux_cmd_red;
union AUX_Cmd_u aux_cmd_grn;

AUX_Ctr_t aux_ctl_red;
AUX_Ctr_t aux_ctl_grn;

bool EnableArrayProcessing = true;

/************************************************************************/
/*                                                                      */
/************************************************************************/
void smart_off_red(void) {
	if ((ee.aux.bcklight_type == BCK_TYPE_ON) && (ee.aux.bcklight_color == BCK_COLOR_RED))
		on_red_l();
	else
		off_red();
}

/************************************************************************/
/*                                                                      */
/************************************************************************/
void smart_off_green(void) {
	if ((ee.aux.bcklight_type == BCK_TYPE_ON) && (ee.aux.bcklight_color == BCK_COLOR_GRN))
		on_grn_l();
	else
		off_grn();
}

/************************************************************************/
/*                                                                      */
/************************************************************************/
//bool is_aux_cmd(void) {
//	return as_red.ubits.enable;
//}

/************************************************************************/
/*                                                                      */
/************************************************************************/
//bool is_aux_beacon(void) {
//	return (as_red.ubits.idmode == IDM_BEACON);
//}

/************************************************************************/
/*                                                                      */
/************************************************************************/
//bool is_aux_voltage(void) {
//	return (as_red.ubits.idmode == IDM_VWARNING);
//}

/************************************************************************/
/*                                                                      */
/************************************************************************/
//bool is_aux_strobe(void) {
//	return (as_red.ubits.idmode == IDM_STROBE);
//}

/************************************************************************/
/*                                                                      */
/************************************************************************/
//void off_aux_cmd(bool rcc_stop) {
//	as_red.ubits.enable = false;
//	as_red.ubits.period = 0;
//	as_red.acmd.aux_command = 0;
//	as_red.ubits.idmode = IDM_OTHER;
//	if (rcc_stop) {
//		//stop_wakeup();
//	}
//	smart_off_red();
//	smart_off_green();
//}

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
	set_aux_cmd(INFO_RED, AUX_60MS, true, IDM_OTHER);
}

void aux_cmd_info_grn(void) {
	set_aux_cmd(INFO_GRN, AUX_60MS, true, IDM_OTHER);
}

//void set_aux_cmd(uint32_t command, AUX_Tick_t delay, bool enable, IDM_Type_t idm_mode) {
//	as_red.acmd.aux_command = command;
//	as_red.ubits.enable = enable;
//	as_red.ubits.idmode = idm_mode;
//	AUX_Set_Delay(delay);							
//}

static void SetBrightness(LED_Bits_t device, uint8_t brightness) {
	switch (device) {
		case RED_LED:
			if (brightness) on_red_h();
			else smart_off_red();
			break;
		case GRN_LED:
			if (brightness) on_grn_h();
			else smart_off_green();
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

void handle_aux_cmd(union AUX_Cmd_u *aux, AUX_Ctr_t *ubits) {

	if (ubits->enable) {

		if (ubits->period == PER_INIT) {
			// ИНИЦИАЛИЗАЦИЯ
			ubits->work_duration = aux->aux.duration0;				// Загружаем длительность.
			ubits->work_count = aux->aux.count;
			ubits->period = PER_FIRST;
		}

		LED_Bits_t device = (LED_Bits_t) aux->aux.device;				// Определяем, чем управлять

		uint8_t current_brightness, brightness;
		uint16_t duration;

		if (ubits->period == PER_FIRST) {
			// ОБРАБАТЫВАЕМ НУЛЕВОЙ ПОЛУПЕРИОД
			brightness = (uint8_t) aux->aux.brightness0;				//... определяем какую яркость установить...

			uint8_t current_brightness = ubits->current_brightness;

			if (brightness != current_brightness) {						// если яркость изменилась - устанавливаем ее.
				SetBrightness(device, brightness);
				ubits->current_brightness = brightness;
			}

			duration = (uint16_t) (ubits->work_duration);				//... на какое время.

			if (--duration) {											// если длительность 0 - закончилась переходим ко второму полупериоду
				ubits->work_duration = duration;						// записываем новое уменьшенное значение
			} else {
				ubits->period = PER_SECOND;								// Переключаем период.
				ubits->work_duration = aux->aux.duration1;				// загружаем значение для следующего полупериода
			}
		}

		if (ubits->period == PER_SECOND) {
			// ОБРАБАТЫВАЕМ ВТОРОЙ ПОЛУПЕРИОД
			brightness = (uint8_t) aux->aux.brightness1;

			current_brightness = ubits->current_brightness;

			if (brightness != current_brightness) {						// если яркость изменилась - устанавливаем ее.
				SetBrightness(device, brightness);
				ubits->current_brightness = brightness;
			}

			duration = (uint16_t) (ubits->work_duration);

			if (--duration) {											// если длительность 0 - закончилась переходим ко второму полупериоду
				ubits->work_duration = duration;						// записываем новое уменьшенное значение
			} else {
				ubits->period = PER_END;								// Досчитали второй полупериод. Переключаем период.
			}
		}

		// РАБОТАЕМ СО СЧЕТЧИКОМ
		uint8_t cycle = (uint8_t) aux->aux.cycle;						// достаем признак вечного цикла
		uint8_t counter = (uint8_t) ubits->work_count;					// Достаем счетчик

		if (ubits->period == PER_END) {									// Два полупериода досчитано - пора решать, что делать дальше.

			ubits->work_duration = aux->aux.duration0;					// Загружаем длительность для следующего цикла.
			ubits->period = PER_FIRST;									// На первый полупериод

			if (!cycle) {												// уменьшаем счетчик, если цикл не вечный
				counter--;
				ubits->work_count = counter;

				if (!counter) {											// Если счетчик досчитал до 0 - отключаем.
					ubits->enable = false;
					ubits->period = PER_NEXT;							// Сообщаем, что пора загружать новую команду.
				}
			}
		}
	}
}

/************************************************************************/
/*                                                                      */
/************************************************************************/
void test_aux_beacon(void) {

//	aux_cmd_red.aux.brightness0	= 0;
//	aux_cmd_red.aux.brightness1	= 0;
//	aux_cmd_red.aux.count		= 7;
//	aux_cmd_red.aux.cycle		= false;
//	aux_cmd_red.aux.device		= RED_LED;
//	aux_cmd_red.aux.duration0	= 5;//8;
//	aux_cmd_red.aux.duration1	= 5;//128;

	uint32_t *p = (uint32_t*)&aux_ctl_red;
	*p = 0;
	p = (uint32_t*)&aux_ctl_grn;
	*p = 0;

	aux_ctl_red.idmode			= IDM_BEACON;
	//aux_ctl_red.arr_index		= 0;
	aux_ctl_red.enable			= true;

//	aux_cmd_grn.aux.brightness0	= 1;
//	aux_cmd_grn.aux.brightness1	= 0;
//	aux_cmd_grn.aux.count		= 5;
//	aux_cmd_grn.aux.cycle		= true;
//	aux_cmd_grn.aux.device		= GRN_LED;
//	aux_cmd_grn.aux.duration0	= 4;//8;
//	aux_cmd_grn.aux.duration1	= 40;//128;

	aux_ctl_grn.idmode			= IDM_BEACON;
	//aux_ctl_grn.arr_index		= 0;
	//aux_ctl_grn.enable			= true;
}

union AUX_Cmd_u *handle_aux_array(const uint32_t pArray[], AUX_Ctr_t *ctr) {

	union AUX_Cmd_u *p = (union AUX_Cmd_u*) pArray;

	p += ctr->arr_index;

	if (p->cmd.is_cmd == 0) {					// если 0 это команда для обработчика массива, а не моргалки

		switch ((Array_Cmd_t) p->cmd.command) {
		case AC_STOP:							// остановить выполнение комманд
			EnableArrayProcessing = false;		// Выключаем
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
	if (EnableArrayProcessing) {
		p_red = handle_aux_array(buf_red, &aux_ctl_red);
		p_grn = handle_aux_array(buf_grn, &aux_ctl_grn);
	}
	handle_aux_cmd(p_red, &aux_ctl_red);
	handle_aux_cmd(p_grn, &aux_ctl_grn);
}

/************************************************************************/
/*                                                                      */
/************************************************************************/
void start_aux_beacon(void) {
	uint32_t cmd = 0;
	uint8_t beacon_type = ee.aux.beacon_type;					// чем моргаем
	uint16_t beacon_mode = (uint16_t) ee.aux.beacon_mode;		// частота моргания

	cmd = (0b00000000000000010000000100001000 | beacon_type | ((beacon_mode + 7) << 12));
	set_aux_cmd(cmd, AUX_1S, true, IDM_BEACON);
}

/************************************************************************/
/*                                                                      */
/************************************************************************/
void stop_aux_beacon(void) {
	if (is_aux_beacon())
		off_aux_cmd(false);
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
	set_aux_cmd((0b00000000000000010000000000000000 | low16), AUX_1S, true, IDM_VWARNING);
}
#endif

/************************************************************************/
/*                                                                      */
/************************************************************************/
void stop_aux_voltage(void) {
	if (is_aux_voltage())
		off_aux_cmd(false);
}

/************************************************************************/
/*                                                                      */
/************************************************************************/
void slot_batt_ind(void) /* в массив настраиваемых слотов */
{
#ifdef USE_BATTIND
	stop_aux_beacon();					// 20.03.2021
	set_aux_count(bat_get_flashes());
#endif
}

/************************************************************************/
/*                                                                      */
/************************************************************************/
void stop_aux_strobe(void) {
//	if (is_aux_strobe())
//		off_aux_cmd(false);
}

void aux_on(void) {
	set_aux_cmd(INFO_GRN, AUX_60MS, true, IDM_OTHER);
}

void aux_off(void) {
	set_aux_cmd(INFO_RED, AUX_60MS, true, IDM_OTHER);
}

void aux_count(uint8_t count) {
	uint32_t cmd;
	if (count)
		cmd = 0b00000000000000010100000100000010 | (count << 4);	// зеленые, если не ноль
	else
		cmd = 0b00000000000000010010001000010001;					// одна красная, если 0
	set_aux_cmd(cmd, AUX_250MS, true, IDM_OTHER);
}

/**********************************************************************************************/

