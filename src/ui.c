/*
 * ui.c
 *
 *  Created on: Dec 24, 2022
 *      Author: tamag
 */

#include <stdbool.h>

#include "ui.h"
#include "array.h"
#include "aux_leds.h"
#include "wl_flash.h"
#include "dac.h"
#include "timers.h"
#include "limiters.h"

#include "main.h"

uint8_t mode_default = DEFAULT_MODE; // режим, в котором стартуем по одному клику.

uint8_t VLPT = LONG_HOLD_UI1; // переменная
DRIVER_Mode_t driver_mode = MODE_OFF;
bool stop_bit = false;

uint16_t step_tc; // step в котором драйвер светит в данный момент c учетом ТК
uint16_t step_tactic;
uint16_t step_user;
uint16_t max_step = MAX_STEP;
uint16_t step_default = DEFAULT_STEP;

RUMP_Dir_t direction = DIR_UP; // 1 - up, 0 - down

PSTEP_t ps; // храним то что было до двойного клика (включения макимума)

uint8_t need_save = false;

/******************************************************************************************/

/******************************************************************************************/

void ON_POWER()
{
	EE_PRINTF("[UI] ON POWER.\r\n");
	dac_pwrpin_on();
	i2c_init(I2C_SPEED_400K);
	for (volatile uint32_t i = 0; i < 24000; i++);  /* ~5мс — время включения DAC7571 */
}

void OFF_POWER()
{
	EE_PRINTF("[UI] OFF POWER.\r\n");
	i2c_deinit();
	dac_pwrpin_off();
}

void ee_defaults(struct ee_data *e)
{
	/* Инженерное меню */
	e->eng.min_level = 1;
	e->eng.vc = 0;
	e->eng.tc = 0;

	/* Интерфейс */
	e->uif.ui_index = 1;
	e->uif.use_memory = 0;

	/* Подсветка */
	e->aux.bcklight_color = BCK_COLOR_RED;
	e->aux.bcklight_type = BCK_TYPE_ON;

	/* Маяк */
	e->aux.beacon_mode = BCON_1S;
	e->aux.beacon_on = true;
	e->aux.beacon_type = GRN_LED;

	/* Пользовательские настройки */
	e->usr.eaux_onoff = false;
	e->usr.id_plus_sid = false;
	e->usr.t_calibr = 6;

	/* Значения UI по умолчанию */
	e->default__ui1 = DEFAULT_STEP;
	e->default__ui2 = DEFAULT_MODE;

	/* Распределение функций по слотам */
	e->slots[0] = SLOT_BATT_IND;
	e->slots[1] = SLOT_BEACON;
	e->slots[2] = SLOT_STROBE;
	e->slots[3] = SLOT_TACTICAL;
	e->slots[4] = SLOT_BLOCK;
	e->slots[5] = SLOT_EMPTY;
	e->slots[6] = SLOT_EMPTY;
	e->slots[7] = SLOT_MEMORY;
	e->slots[8] = SLOT_BACKLIGHT;
	e->slots[9] = SLOT_IPSMODE;
}

uint16_t get_step_tc(uint16_t step)
{
	int16_t tc_step = (MAX_STEP - limiter); // учитываем термоконтроль - отнимаем от заданного шага корреецию
	uint16_t result = step <= tc_step ? step : (tc_step > 0 ? tc_step : 1);
	//EE_PRINTF("[UI] get_step_tc step=%u tc=%u -> %u\r\n", step, tc_step, result);
	return result;
}

/******************************************************************************************/

uint16_t get_min_level(uint16_t step)
{																   // если step = 0 возвращаем 0, иначе возвращаем минимальный уровень
	return step ? ((ee.eng.min_level << 1) + BASE_MOON_LEVEL) : 0; // BASE_MOON_LEVEL прибавляется чтобы уложиться в запись только 4 бит.
}

/******************************************************************************************/

uint16_t getBright_CRT_9to12Bit(uint16_t val) {
    // Адаптированная начальная полка для 9-битного диапазона
    if (val < 2)  return val; // Мгновенный микро-старт (0, 1, 2, 3)
    if (val < 10) return 1;   // Удержание минимального тления
    
    // Жесткая посадка на максимум (100% ШИМ)
    if (val >= 511) return 4095; 

    uint32_t x = val;
    
    // Кубический разгон без переполнения uint32_t
    // Максимум при val=510: 510^3 * 16 = 2 122 416 000 (Лимит 4.29 млрд не превышен!)
    // 2 122 416 000 >> 19 = 4048
    uint32_t result = (x * x * x * 16) >> 19;

    // Коррекция, чтобы избежать случайного нуля после сдвига на малых val
    if (result == 0) result = 1; 

    return (uint16_t)result;
}

void out_set_stc(uint16_t step)
{
	uint16_t out = getBright_CRT_9to12Bit(step) + get_min_level(step);
	//if (out > 4095) out = 4095;		// не удалять, блин - надо! С новым I2C не надо
	//EE_PRINTF("[UI] out_set_stc step=%u out=%u\r\n", step, out);
	dac7571_set_voltage(out);
}
/******************************************************************************************/

uint8_t on_driver(void) // Если драйвер выключен - включаем и вернем true, если выключен ничего не делаем и возвращаем false
{
	EE_PRINTF("[UI] ON driver.\r\n");

	if (ee.uif.ui_index)
		step_tactic = 0;

	if (driver_mode < MODE_ON)
	{
		// save_bckl_status();
		ON_POWER();
		vwarning = VWARNING_NONE;	  //
		vwarning_old = VWARNING_NONE; // 08.04.2020
		direction = DIR_UP;
		driver_mode = MODE_ON;
		return true;
	}
	else
		return false;
}

/************************************************************************/
void off_driver(uint8_t reset_step, SoftStartSpeed_t ss_speed)
{
	EE_PRINTF("[UI] OFF driver.\r\n");

	// restore_bckl_status();

	driver_mode = MODE_OFF;
	direction = DIR_UP;

	set_current(0, ss_speed);

	reset_limiters();											// это для ТС	!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

	if (ps.use_prev) // выполняется в случае если турбо было выключено.
	{
		step_tc = 0;		 // TC по любому обнуляем
		ps.use_prev = false; // сбрасываем флаг
		if (!ps.start)		 // если мы не стартовали драйвер...
		{
			if (ee.uif.ui_index == 1) // ... то есть смысл восстановить предыдущий шаг, если это рампинг...
				step_default = ps.pr_step;
			else
				// ... и режим, если это дискрет
				mode_default = ps.pr_step;
		}
		step_tactic = MAX_STEP; // step_tactic ставим в макимально возможный, это для тактического режима, если step_tactic = 0 будет использоваться step_default
	}

#ifdef USE_EX_AUX
	stop_aux_voltage(false); // отключаем моргания о низкой напруге
#endif

	// sleep_remains = SLEEP_DELAY;
	if (reset_step)
		step_tc = 0;
}

/******************************************************************************************/
void set_current_ll(uint16_t step_target, SoftStartSpeed_t ss_speed)
{
	if (!step_target && step_tc)
		step_default = step_tc; // шаг перед выключение нужен для СидПлюсИд и включение в предыдущем режиме.

	if (ss_speed != SS_NO)
		ss_init(step_target, step_tc, ss_speed);
	else
	{
		out_set_stc(step_target); // Всегда!!! Этот step используется в данный момент. В следующий раз он уже будет старым.
		step_tc = step_target;
	}
}

void set_current(uint16_t step_target, SoftStartSpeed_t ss_speed)
{
	//EE_PRINTF("[UI] set_current step=%u ss=%u\r\n", step_target, ss_speed);
	step_user = step_target; // для ТК. step_user это то, что по идее хочет пользователь
	set_current_ll(step_target, ss_speed);
	if((!step_target) && (!ss_speed )) OFF_POWER();
}

/******************************************************************************************/

uint16_t get_max_step(void)
{
	return MAX_STEP;
}

void off_auto_repeat(void)
{
	if (Pressed)
		stop_bit = true;
}

void step_up_ui1(void)
{
	if (step_user < max_step)
		set_current(++step_user, SS_NO);
}

void step_down_ui1(void)
{
	if (step_user > 1)
		set_current(--step_user, SS_NO); // не даем погасить совсем, по этому не 0, а 1
}

void on_rumpup_ui1(void)
{
	//EE_PRINTF("[UI] ramp UP  step=%u dir=%u\r\n", step_user, direction);
	on_driver(); // заводим если надо
	ps.use_prev = false;
	if (direction)
		step_up_ui1();
	else
		step_down_ui1();
}

void on_rumpdown_ui1(void)
{
	//EE_PRINTF("[UI] ramp DOWN step=%u\r\n", step_user);
	ps.use_prev = false;
	step_down_ui1();
}

void on_xmode_ui1(void)
{
	//EE_PRINTF("[UI] X-mode step=%u mem=%u\r\n", step_user, ee.uif.use_memory);
	ps.use_prev = false; //! НЕ УБИРАТЬ - НАДО!
	on_driver();
	set_current(ee.uif.use_memory ? (step_user ? step_user : step_default) : ee.default__ui1, SS_FAST); // если предыдущий шаг = 0 начинаем с яркости по умолчанию, иначе - с яроксти в пред. шаге
}

void on_turbo_ui1(void)
{
	//EE_PRINTF("[UI] turbo step=%u max=%u\r\n", step_user, max_step);
	uint8_t start = on_driver();

	if (!ps.use_prev) // включаем турбо. если до этого драйвер был выключен - start будет равен true
	{
		ps.start = start;								   // true только если завели драйвер т.е. до двух кликов он был выключен
		ps.pr_step = step_user ? step_user : step_default; // запоминаем текущий шаг если он не ноль. если он ноль - запоминаем запасное значение
		ps.use_prev = true;								   // ставим флаг использовать этот шаг при выходе из турбо
		set_current(max_step, SS_FAST);					   // плавно включаем турбо, но АКБ уже не та и корпус может быть горячим, по двойному клику тоже уже не MAX_STEP а только max_step
	}
	else // выключаем турбо. Тут два варианта нужно или вернуть прежнее значение или выключить драйвер. Зависит от ps.start
	{
		ps.use_prev = false; // сбрасываем флаг.

		if (ps.start)				   // Драйвер был включен двойным кликом. Выключаем.
			off_driver(true, SS_FAST); // обнуляем степ и выключаем
		else						   // иначе - драйвер уже был включено. Возвращаем яркость
		{
			direction = DIR_UP;
			set_current(ps.pr_step, SS_FAST); // вернулись из максимума и вернули шаг
		}
		step_default = ps.pr_step; // Потом Будет включено в этой яркости. Обязательно должно быть в конце.
	}
}

void off_driver_false(void)
{
	EE_PRINTF("[UI] OFF driver (false)\r\n");
	driver_mode = MODE_OFF;
	// mode_default = mode_current;
	// mode_current = 0;
	off_driver(false, SS_FAST);
}

/******************************************************************************************/

uint8_t execute_func(void (*func_ptr)(void), uint8_t timeout, uint8_t check_timeout)
{
	if (check_timeout && !timeout)
		return false;
	func_ptr();
	return true;
}

/************************************************************************/
/*		                   [UI1/2] общий код							*/
/************************************************************************/

union common_commands
{
	struct __attribute__((packed))
	{
		unsigned int inp_cout : 3;
		unsigned int is_hold : 1;
		unsigned int inp_mask : 6;
		unsigned int drv_mode : 5;
		unsigned int ui : 1;
	} bit;
	uint16_t byte16;
};

uint8_t check_bit_map_ui(uint8_t bt_result, uint8_t bt_cnt, uint8_t v_long, uint8_t timeout)
{

	/////////////////////////////////////////
	// 15-й  бит	- выбор UI
	// 14-10 биты	- режим, в котором работаем
	// 09-04 биты	- маска нажатий
	// 03-й  бит	- признак удержания
	// 02-00 биты	- счетчик длинны ввода
	/////////////////////////////////////////

	uint8_t op_index = 0;
	uint8_t check_timeout = 0;
	union common_commands cc;

	void (*func_ptr)(void);
	func_ptr = 0;

	//__NOP();
	/* БЫСТРЫЙ способ (правильные смещения): */
	cc.byte16 = (((uint16_t)bt_cnt << 0) |				 // bits 0-2
				 ((uint16_t)v_long << 3) |				 // bit 3
				 ((uint16_t)bt_result << 4) |			 // bits 4-9
				 ((uint16_t)driver_mode << 10) |		 // bits 10-14
				 ((uint16_t)(ee.uif.ui_index > 1) << 15) // bit 15
	);

	/*	uint8_t uni_slots(uint8_t index, uint8_t timeout)
	 {
	 if(!timeout) off_auto_repeat();
	 op_index = ee.slots[index];
	 return timeout;
	 }
	 */

	switch (cc.byte16)
	{
	/************************************************************************/
	/*                          UI1                                         */
	/************************************************************************/

	// из MODE_OFF
	case bit_map_on_ui1:
		func_ptr = &on_xmode_ui1;
		check_timeout = true;
		break;

	case bit_map_rumpup_ui1:
		VLPT = LONG_HOLD_RAMP_PAUSE_UI1; // пауза сразу после включения в минимум. Служит для удобства попадания в минимум.
										 // no break
										 // из MODE_ON
	case bit_map_rumpup_on_ui1:			 // включить в самый минимум
		func_ptr = &on_rumpup_ui1;		 // ступенька вверх удержанием
		check_timeout = false;
		break;

	case bit_map_rumpdown_ui1:
		func_ptr = &on_rumpdown_ui1; // ступенька вниз удержанием
		check_timeout = false;
		break;

	case bit_map_turbo0_ui1:
	case bit_map_turbo1_ui1:
		func_ptr = &on_turbo_ui1;
		check_timeout = true;
		break;

	case bit_map_off_ui1:
		// case bit_map_off_ui2:						// OFF
		func_ptr = &off_driver_false;
		check_timeout = true;
		break;

	default:
		return 0;
		break;
	}

	void (*target_func)(void) = func_ptr ? func_ptr : main_op[op_index];
	uint8_t result = execute_func(target_func, timeout, check_timeout);
	return result;
}

/************************************************************************/
/*							[UI1] скан рампинг                          */
/************************************************************************/

void scan_ui1(void) // Эта функция сканер. Она должна вызываться.
{
	static uint8_t bt_r_time = 0; // Счетчик сколько времени прошло после отпускания кнопки
	static uint8_t bt_d_time = 0; // Переменная времени нажатия кнопки
	static uint8_t bt_u_time = 0; // Переменная времени отпускания кнопки
	static uint8_t bt_cnt = 0;	  // Переменная счетчик нажатий
	static uint8_t flag = false;  // флаг устанавливается при обработке сверх длинного нажатия. Дабы не отрабатывать отпускание кнопки

	if (Pressed) // Кнопень придавлена.
	{
		bt_u_time = 0;
		tick_5s_reset();  // сброс таймера сна — не засыпаем при рампинге

		if (!bt_d_time)			// если переменная 0 кнопка только нажата
		{
			max_step = get_max_step(); // - (pic.correction + 1);			// вычисляем max_step в зависимости от напряжения на АКБ и перегрева
									   //			stop_aux_beacon();								// отключаем маяк чтобы не моргал перед отмаргиванием АКБ
		}

		if ((!bt_d_time) && (step_tc >= max_step)) // если мы уже вверху и кнопка только нажата...
			direction = DIR_DOWN;				   // ...меняем направление на вниз

		if (bt_d_time < 0xFF)
			bt_d_time++; // увеличиваем счетчик времени нажатого состояния если это не переполнит uint8_t

		// if ((bt_d_time == 1) && (driver_mode < MODE_BLOCK_C))
		// start_aux_cmd(SHORT_BLINK, AWU_15MS, true, IDM_OTHER);// моргаем красным при нажатии на кнопку в режиме блокировки

		if (bt_d_time >= VLPT)							// это для попадания в минимум в начале рампинга.
		{
			flag = true;
			VLPT = 1; // BASE_RUMP_SPEED;					// быстрое время для рампинга. в настройках 0 или 1, а нам надо 2 или 3, по этому прибавляем 2
			check_bit_map_ui(0, bt_cnt, true, false);
			if (!stop_bit)
				bt_d_time = 1; // для переключения режимов удержанием вводим автоповтор и способ его отключить.
		}
	}

	else
	{
		VLPT = LONG_HOLD_UI1; // при отпускании кнопки возвращаем тайминг

		if (bt_d_time > 0)
		{ // Момент отпускания кнопки.
			// sleep_remains = SLEEP_DELAY;		// сбрасываем при отпускании... надо ли если сбрасываем при нажатой???
			stop_bit = false;
			direction ^= 0x01; // кнопку отпустили, но на случай, если быстро нажмут опять меняем направление рампинга.
			bt_r_time = 0;
			bt_cnt++; // Короткое нажатие. Пропускаем установку бита.

			if (flag) {		// здесь ковырялось ИИ
				bt_cnt = 0;
				flag = false;
			} else if (check_bit_map_ui(0, bt_cnt, false, false) || bt_cnt == COMM_MAX_LEN_UI1) {
				reset_res_ui3();
			}

			bt_d_time = 0; // время придавки кнопки нам больше не нужно, сбрасываем.
		}

		else if (bt_cnt && (++bt_u_time >= RELEASE_TIMEOUT_UI1))
		{ // Отпущено, счетчик таймаута комманды(последовательности).
			if (bt_cnt > 0)
			{ // Таймаут комманды, сброс состояний
				check_bit_map_ui(0, bt_cnt, false, true);
				reset_res_ui3();
			}
		}

		if (bt_r_time < DIRECTION_TIMEOUT_UI1)
			bt_r_time++;
		else
			direction = DIR_UP; // после выжидания таймаута меняем направление рампинга
	}
}
