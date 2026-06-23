#ifndef UI_H_
#define UI_H_

/*
 * ui.h — Пользовательский интерфейс (UI) — обработка кнопки и рампинг яркости
 *
 * Модуль обрабатывает нажатия кнопки и управляет яркостью LED:
 *   - scan_ui1()      — сканер рампинга (вызывается каждые ~7мс)
 *   - set_current()   — установка яркости с учётом термоконтроля
 *   - on_turbo_ui1()  — турбо-режим (двойной клик)
 *   - on_rampup_ui1() — рампинг вверх (удержание кнопки)
 *
 * Режимы работы (DRIVER_Mode_t):
 *   MODE_OFF      — фонарь выключен
 *   MODE_ON       — фонарь включён
 *   MODE_STROB    — стробоскоп
 *   MODE_BLOCK    — блокировка
 *
 * Битовые маски команд (bit_map_*_ui1):
 *   Кодируются: режим + удержание + количество кликов
 */
#define UI_H_

#include "debug.h"
#include "soft_start.h"

extern u_int16_t crt;
extern uint16_t step_default;
extern uint16_t step_user;
extern uint16_t step_tc;

typedef struct {
	uint16_t  start;
	uint16_t  pr_step;
	uint16_t  use_prev;
} PSTEP_t;

#define BASE_MOON_LEVEL 5
#define DEFAULT_STEP 128			// Шаг для УИ1.  Применяется при начальной инициализации.
#define DEFAULT_MODE CMODE_MID		// Моде для УИ2. Применяется при начальной инициализации.

#define KEY_PORT	GPIOC
#define KEY_PIN		GPIO_Pin_3

#define Released		(KEY_PORT->INDR & KEY_PIN)
#define Pressed			(!Released)

/*******************************************************************************************************/

#define reset_res_ui3()		do { bt_cnt=0; flag = false; } while (0);

typedef enum {
	MODE_BLOCK		=	0U,		// Режим блокировки
	MODE_BLOCK0,				// Режим блокировки, в котором работает маяк
	MODE_BLOCK_C,				// Режим блокировки во включенном режиме
	MODE_OFF,					// фонарь выключен, если ничего не жать уснем
	MODE_ON,					// фонарь включен и работает
	MODE_STROB,					// включены стробы

	MODE_RAMP_SPEED = 10U,		// режим регулироки скорости рампинга 10
	MODE_MOON_LEVEL,			// режим настройки минимального уровня мунлайта
	MODE_PIP_TIME,				// режим настройки временной оставляющей PIP-регулятора

	MODE_CTK_A,					// пользовательская калибровка термоконтроля нагревом
	MODE_CTK_H,					// пользовательская калибровка термоконтроля заданием значения
	MODE_MAGIC,					// вход в режим настройки слотов
	MODE_UIS,					// режим выбора UI
	MODE_BM,					// переключение частоты вспышек маяка
	MODE_BT,					// выбор чем маяковать
	MODE_BL,					// выбор чем маяковать

	MODE_SLOT01,
	MODE_SLOT02,
	MODE_SLOT03,
	MODE_SLOT04,
	MODE_SLOT05,
	MODE_SLOT06,
	MODE_SLOT07,
	MODE_SLOT08,
	MODE_SLOT09,
	MODE_SLOT10					//29
} DRIVER_Mode_t;

extern DRIVER_Mode_t driver_mode;

typedef enum {
	CMODE_OFF =	0U,
	CMODE_MOON,
	CMODE_MIN,
	CMODE_MID0,
	CMODE_MID,
	CMODE_MAX,
	CMODE_TURBO,
	CMODE_NOACTION
} CURRENT_Modes_t;

/************************************************************************/
/*                       Рампинг UI                                     */
/************************************************************************/
#define RELEASE_TIMEOUT_UI1			80//40		// Пауза для ожидания продолжения ввода
#define LONG_HOLD_UI1				50//100		// удержание - реакция не дожидаясь отпускания (таймаут для рампинга)
#define LONG_HOLD_RAMP_PAUSE_UI1	120//93		// Пауза на step=1 рампинга из выключено. Для удобства попадания в минимальный.
#define DIRECTION_TIMEOUT_UI1		160//93		// Пауза после которой направление рампинга не будет меняться при отпускании и повторном нажатии кнопки.
#define COMM_MAX_LEN_UI1			7		// максимальная длинна ввода

/*************************************************************************/

#define MODES_QTY			7
#define SLEEP_DELAY			100
#define SLEEP_DELAY_BAT		2500

typedef enum {
	DIR_DOWN = 0U,
	DIR_UP
} RUMP_Dir_t;

#define MAX_STEP			511
#define POW_THRD			120

/************************************************************************/
/*                                UI1                                   */
/************************************************************************/
/////////////////////////////////////////
// 07-04 биты	- режим, в котором работаем
// 03-й  бит	- признак удержания
// 02-00 биты	- счетчик длинны ввода
/////////////////////////////////////////

/************************************************************************/
/*		   команды работающие из "выключено" MODE_OFF                   */
/************************************************************************/

#define  bit_map_on_ui1		0b0000110000000001		// режим = OFF 1 кк - включение
#define  bit_map_rumpup_ui1	0b0000110000001000		// режим = OFF включить в минимально возможном и начать увеличивать до конца удержания
#define  bit_map_turbo0_ui1	0b0000110000000010		// режим = OFF 2 кк - включение в турбо

#define	 bit_map_1ch_ui1	0b0000110000001001		// режим = OFF 1 кк + удерж - включение отмаргивателя АКБ
#define  bit_map_2ch_ui1	0b0000110000001010		// режим = OFF 2 кк + удерж - включение выключение маяка

#define  bit_map_3c_ui1		0b0000110000000011		// режим = OFF 3 кк - стробы
#define  bit_map_3ch_ui1	0b0000110000001011		// режим = OFF 3 кк + удерж

#define  bit_map_4c_ui1		0b0000110000000100		// режим = OFF BLOK 4 кк
#define  bit_map_4ch_ui1	0b0000110000001100		// режим = OFF BLOK0 4 кк + удерж.

#define bit_map_5c_ui1		0b0000110000000101		// режим = OFF, TACTICAL MODE 5 кк
#define bit_map_5ch_ui1		0b0000110000001101		// режим = OFF, TACTICAL MODE 5 кк + удерж

#define bit_map_6c_ui1		0b0000110000000110		// режим = OFF, переключние подсветки 6 кк
#define bit_map_6ch_ui1		0b0000110000001110		// режим = OFF, переключние подсветки 6 кк + удерж

/************************************************************************/
/*		   команды работающие из "включено" MODE_ON                     */
/************************************************************************/

#define  bit_map_off_ui1		0b0001000000000001		// режим = ON один кк - выключение

#define  bit_map_rumpup_on_ui1	0b0001000000001000		// режим = ON начать увеличивать до конца удержания
#define  bit_map_rumpdown_ui1	0b0001000000001001		// режим = ON один кк и удержание - рампинг вниз с автоповтором
#define  bit_map_turbo1_ui1		0b0001000000000010		// режим = ON два кк - включение в максимуме

#define  bit_map_5sh_ui1		0b0001000000001101		// режим = ON 5 кк + удержание запомнить текущую яркость как яркость по умолчанию

/************************************************************************/
/*		   команды работающие из "стробы" MODE_STROB                    */
/************************************************************************/

#define  bit_map_strob_off_ui1	0b0001010000000001 		// режим = MODE_STROB один кк
#define  bit_map_strob_up_ui1	0b0001010000000010			// режим = MODE_STROB 2 кк
//#define	 bit_map_strob_back	0b01010011		// режим = MODE_STROB тройной клик

/************************************************************************/
/*	 команды работающие из "заблокировано" MODE_BLOCK                   */
/************************************************************************/

#ifdef USE_MAGIC_SLOTS
#define  bit_map_unblok_ui1		0b0000000000000110		// режим = UNBLOK 6 кк
#else
#define  bit_map_unblok_ui1		0b00000100		// режим = UNBLOK 4 кк
#endif

/************************************************************************/
/*	 команды работающие из "заблокировано" MODE_BLOCK0                  */
/************************************************************************/

#ifdef USE_MAGIC_SLOTS
#define  bit_map_unblok0_ui1		0b0000010000000110		// режим = UNBLOK 6 кк
#else
#define  bit_map_unblok0_ui1		0b00010100		// режим = UNBLOK 4 кк
#endif

/************************************************************************/
/*                                /UI1                                  */
/************************************************************************/

void off_driver_false(void);
void step_up_ui1(void);
void step_down_ui1(void);
void on_rumpup_ui1(void);
void on_rumpdown_ui1(void);
void on_xmode_ui1(void);
void on_turbo_ui1(void);

void scan_ui1(void);

void set_current_ll(uint16_t step_target, SoftStartSpeed_t ss_speed);
void set_current(uint16_t step_target, SoftStartSpeed_t ss_speed);
uint16_t get_step_tc(uint16_t step);
void out_set_stc(uint16_t step);

void ON_POWER(void);
void OFF_POWER(void);

#endif /* UI_H_ */
