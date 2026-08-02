/**
 * @file aux_leds.c
 * @brief AUX LED control module (red, green, backlight)
 *
 * Supports two operating modes:
 *   1. Single command (start_aux_cmd) — one blink sequence
 *   2. Command array (start_aux_array) — sequence of commands from Flash
 *
 * handle_aux() is called every ~15 ms (AWU_15MS) for step-by-step processing.
 *
 * 32-bit command format (union AUX_Cmd_u):
 *   [2:0]   device    — channel: 1=RED, 2=GRN, 4=HBL
 *   [3]     cycle     — 0=single, 1=looped
 *   [7:4]   count     — repeats (0..15)
 *   [10:8]  p0.psc    — prescaler 1st half-period (0..7 -> 128..61440)
 *   [16:11] p0.win    — window 1st half-period (0..63)
 *   [19:17] p1.psc    — prescaler 2nd half-period
 *   [25:20] p1.win    — window 2nd half-period (0..63)
 *   [28:26] brightness0 — modes[] index 1st half-period
 *   [31:29] brightness1 — modes[] index 2nd half-period
 *
 * Array command elements:
 *   Each element is either an LED command (is_cmd != 0)
 *   or a system command (is_cmd == 0):
 *     AC_STOP        — stop array processing
 *     AC_REP_ATSTART — loop from start of array
 *     AC_REP_ATSTEPS — loop from specified step
 *
 * Created: 22.02.2020
 * Modified: 15.06.2026
 * Author: Tamagotchi
 */

#include "aux_leds.h"
#include "main.h"
#include "led_gpio.h"
#include "app_data.h"
#include "adc.h"
#include "ui.h"
#include <string.h>

uint8_t vwarning = VWARNING_NONE;			// Текущий уровень предупреждения о напряжении
uint8_t vwarning_old = VWARNING_NONE;		// Предыдущий уровень (для отслеживания изменений)

/* Массивы команд для пошаговой обработки (хранятся в Flash).
 * Формат: MAKE_CMD(device, cycle, count, psc0, win0, psc1, win1, brightness0, brightness1)
 * Время полупериода = (psc_значение × win) / 128 мс.
 */
const uint32_t buf_red[5] = {
	MAKE_CMD(RED_LED|HBL_LED, 0, 8, AWU_PSC_128, 45, AWU_PSC_4096, 60, 1, 0),   /* ON 45ms, OFF 1920ms × 8 */
	MAKE_CMD(RED_LED|HBL_LED, 0, 4, AWU_PSC_4096, 60, AWU_PSC_128, 45, 1, 0),   /* ON 1920ms, OFF 45ms × 4 */
	MAKE_CMD(RED_LED|HBL_LED, 0, 4, AWU_PSC_128, 45, AWU_PSC_4096, 60, 1, 0),   /* ON 45ms, OFF 1920ms × 4 */
	MAKE_CMD(RED_LED|HBL_LED, 0, 4, AWU_PSC_4096, 60, AWU_PSC_128, 45, 1, 0),   /* ON 1920ms, OFF 45ms × 4 */
	0x1  /* AC_REP_ATSTART: зациклить весь массив */
};
const uint32_t buf_grn[3] = {
	MAKE_CMD(GRN_LED, 0, 1, AWU_PSC_128, 45, AWU_PSC_256, 30, 1, 0),             /* ON 45ms, OFF 60ms */
	MAKE_CMD(GRN_LED, 0, 1, AWU_PSC_256, 30, AWU_PSC_256, 30, 1, 0),             /* ON 60ms, OFF 60ms */
	0x1  /* AC_REP_ATSTART: зациклить весь массив */
};
const uint32_t buf_sos_red[] = {
	/* S: ··· (count=3) */
	MAKE_CMD(RED_LED, 0, 3, AWU_PSC_256, 63, AWU_PSC_256, 63, 1, 0),             /* ·: ON 126ms, OFF 126ms ×3 */
	/* O: ——— (count=3) */
	MAKE_CMD(RED_LED, 0, 3, AWU_PSC_1024, 47, AWU_PSC_256, 63, 1, 0),            /* —: ON 376ms, OFF 126ms ×3 */
	/* S: ··· (count=3) */
	MAKE_CMD(RED_LED, 0, 3, AWU_PSC_256, 63, AWU_PSC_256, 63, 1, 0),             /* ·: ON 126ms, OFF 126ms ×3 */
	/* Пауза между циклами SOS */
	MAKE_CMD(RED_LED, 0, 0, AWU_PSC_1024, 55, AWU_PSC_1024, 55, 0, 0),            /* OFF 440ms + OFF 440ms = 880мс */
	0x1  /* AC_REP_ATSTART */
};

/* Указатель на текущую AUX-команду (одиночная или из массива).
 * Если NULL — обработчик неактивен.
 * Используется handle_aux() для вызова handle_aux_cmd().
 */
union AUX_Cmd_u *p_aux;

/* Буфер для одиночной команды (start_aux_cmd).
 * Копирует команду из параметра, чтобы она оставалась доступной
 * пока handle_aux() обрабатывает её пошагово.
 */
union AUX_Cmd_u aux_cmd;

/* Единая структура состояния обработчика AUX.
 * Содержит: текущий период, яркость, оставшуюся длительность,
 * счётчик повторов, индекс в массиве, режим индикации.
 */
AUX_Ctr_t aux_ctl;

/* Флаг включения пошаговой обработки массива.
 * true  — handle_aux() вызывает handle_aux_array()
 * false — массив не обрабатывается (одиночная команда или idle)
 */
bool EnableArrayProcessing = false;

/* Указатель на текущий обрабатываемый массив команд.
 * Нужен, чтобы handle_aux() знал какой массив передавать в handle_aux_array().
 */
const uint32_t *current_buf = NULL;

#ifdef USE_STROBE
/* Strobe profiles use the same packed command format as every other AUX
 * sequence.  The old raw levels map to the nearest entries in modes[]:
 * 508 -> TURBO (511), 196 -> MID (220), and 300 -> MAX (360). */
static const uint32_t strobe_commands[STROBE_COUNT] = {
    MAKE_CMD(HBL_LED, 1, 0, AWU_PSC_256, 15, AWU_PSC_256, 15,
             CMODE_TURBO, CMODE_OFF), /* 30ms / 30ms */
    MAKE_CMD(HBL_LED, 1, 0, AWU_PSC_512, 15, AWU_PSC_10240, 25,
             CMODE_TURBO, CMODE_MID), /* 60ms / 2s */
    MAKE_CMD(HBL_LED, 1, 0, AWU_PSC_512, 15, AWU_PSC_2048, 63,
             CMODE_MAX, CMODE_OFF), /* 60ms / ~1s */
    MAKE_CMD(HBL_LED, 1, 0, AWU_PSC_512, 15, AWU_PSC_10240, 50,
             CMODE_MAX, CMODE_OFF), /* 60ms / 4s */
};
#endif

/**
 * @brief Запуск пошаговой обработки массива команд
 * @param pArray — указатель на массив uint32_t команд в Flash
 * @param delay  — начальная задержка перед первым полупериодом (awu_tick_t:
 *                 AWU_15MS, AWU_120MS, AWU_250MS, AWU_500MS, AWU_1S и т.д.)
 *
 * Сбрасывает структуру состояния, устанавливает флаг EnableArrayProcessing
 * и запоминает указатель на массив для дальнейшей обработки в handle_aux().
 */
void start_aux_array(const uint32_t *pArray, awu_tick_t delay) {
	off_aux_cmd(false);
	memset(&aux_ctl, 0, sizeof(aux_ctl));

	EnableArrayProcessing = true;
	p_aux = (union AUX_Cmd_u*)pArray;
	current_buf = pArray;
	aux_ctl.enable = true;

	AWU_set_delay(delay);
	AWU_ON();
}

/**
 * @brief Умное выключение светодиода с учётом режима подсветки
 * @param channel — битовая маска каналов (RED_LED, GRN_LED, HBL_LED, можно комбинировать)
 *
 * Если подсветка включена (bcklight_type == BCK_TYPE_ON):
 *   - Если цвет подсветки совпадает с каналом — включает слабый режим (on_*_l)
 *   - Иначе — полное выключение (off_*)
 * Если подсветка выключена — полное выключение.
 */
void smart_off_leds(LED_Bits_t channel) {
	if (EE.aux.bcklight_type != BCK_TYPE_ON) {
		if (channel & RED_LED) off_red();
		if (channel & GRN_LED) off_grn();
		return;
	}

	if (channel & RED_LED) {
		(EE.aux.bcklight_color == BCK_COLOR_RED) ? on_red_l() : off_red();
	}
	if (channel & GRN_LED) {
		(EE.aux.bcklight_color == BCK_COLOR_GRN) ? on_grn_l() : off_grn();
	}
}

/**
 * @brief Проверка режима индикации
 * @param mode — проверяемый режим (IDM_BEACON, IDM_VWARNING, IDM_STROBE, IDM_OTHER)
 * @return true если обработчик активен и работает в указанном режиме
 */
bool is_aux_mode(IDM_Type_t mode) {
	return (aux_ctl.enable && (aux_ctl.idmode == mode));
}

/**
 * @brief Проверка необходимости DAC/I2C для активной AUX-команды
 *
 * Используется после пробуждения AWU/STANDBY для восстановления периферии
 * перед изменением состояния HBL или strobe, которое затрагивает вывод DAC.
 *
 * @return true если активная команда использует HBL_LED или strobe режим
 */
bool aux_needs_dac(void)
{
	return aux_ctl.enable && p_aux &&
	       (((LED_Bits_t)p_aux->aux.device & HBL_LED) != 0u);
}

/**
 * @brief Проверка активности
 * @return true если обработчик активен (enable == true)
 */
bool is_aux_active(void) {
	return aux_ctl.enable;
}

/**
 * @brief Полная остановка AUX-обработчика
 * @param stop_lsi — если true, остановить LSI-генератор (AWU)
 *
 * Сбрасывает enable, period, idmode; выключает все светодиоды.
 */
void off_aux_cmd(bool stop_lsi) {
	bool had_dac_output = aux_needs_dac();

	if (had_dac_output) {
		out_set_stc(0u);
	}

	aux_ctl.enable = false;
	aux_ctl.period = PER_INIT;
	aux_ctl.idmode = IDM_OTHER;
	EnableArrayProcessing = false;
	p_aux = NULL;
	current_buf = NULL;

	smart_off_leds(RED_LED | GRN_LED | HBL_LED);

	if (stop_lsi) {
		AWU_stop(true);
	}
}

#ifdef USE_BATTIND
/**
 * @brief Формирование команды индикации количества вспышек
 *
 * @param count — количество (0..6):
 *   0,1 → 1 вспышка RED_LED
 *   2   → 2 вспышки RED_LED
 *   3   → 3 вспышки RED_LED|GRN_LED
 *   4   → 4 вспышки RED_LED|GRN_LED
 *   5   → 5 вспышек GRN_LED
 *   6   → 6 вспышек GRN_LED
 *
 * Если EE.usr.id_plus_sid == 1, к каналу добавляется HBL_LED.
 * Время: ON=75мс, OFF=225мс.
 * @return 32-битная команда для start_aux_cmd()
 */
uint32_t set_aux_count(uint8_t count) {
	LED_Bits_t device;
	uint8_t flashes;

	if (count <= 2) {
		device = RED_LED;
		flashes = count ? count : 1;
	} else if (count <= 4) {
		device = RED_LED | GRN_LED;
		flashes = count;
	} else {
		device = GRN_LED;
		flashes = count;
	}

	if (EE.usr.id_plus_sid)
		device |= HBL_LED;

	return MAKE_CMD(device, 0, flashes, AWU_PSC_256, 38, AWU_PSC_1024, 28, 1, 0);
}
#endif

/**
 * @brief Запуск одиночной AUX-команды
 * @param command   — 32-битная команда
 * @param enable    — true = начать обработку немедленно
 * @param idm_mode  — режим индикации (IDM_BEACON, IDM_VWARNING, IDM_STROBE, IDM_OTHER)
 * @param delay     — начальная задержка перед первым полупериодом (awu_tick_t:
 *                    AWU_15MS, AWU_120MS, AWU_250MS, AWU_500MS, AWU_1S и т.д.)
 *                    После первого пробуждения AWU переключается на psc0/win0 команды.
 *
 * Таймер AWU настраивается в handle_aux_cmd при обработке PER_FIRST.
 */
void start_aux_cmd(uint32_t command, bool enable, IDM_Type_t idm_mode, awu_tick_t delay) {
	#ifdef AUX_DEBUG
	debug_cmd(command);
	#endif

	off_aux_cmd(false);

	aux_cmd.aux_cmd = command;

	aux_ctl.enable = enable;
	aux_ctl.idmode = idm_mode;
	aux_ctl.period = PER_INIT;
	aux_ctl.current_brightness = 0;
	aux_ctl.work_count = 0;

	p_aux = &aux_cmd;

	AWU_set_delay(delay);
	AWU_ON();
}

/**
 * @brief Установка яркости каналов AUX/строба
 * @param device Битовая маска каналов (RED_LED, GRN_LED, HBL_LED).
 * @param brightness Индекс уровня в modes[] для HBL, 0/1 для GPIO LED.
 */
static void SetBrightness(LED_Bits_t device, uint8_t brightness)
{
	if (device & RED_LED) {
		if (brightness) on_red_h();
		else smart_off_leds(RED_LED);
	}
	if (device & GRN_LED) {
		if (brightness) on_grn_h();
		else smart_off_leds(GRN_LED);
	}
	if (device & HBL_LED) {
		/* HBL uses the same discrete brightness index as UI2.  Do not call
		 * set_current(): it changes step_user and can power down the DAC. */
		if (brightness >= CMODE_NOACTION)
			brightness = CMODE_OFF;
		out_set_stc(get_stepi(brightness));
	}
}

#ifdef USE_STROBE
/**
 * @brief Запуск стробоскопа через общий AUX command handler.
 * @param index Strobe profile in range 0..STROBE_COUNT-1.
 */
void start_aux_strobe(uint8_t index)
{
	if (index >= STROBE_COUNT)
		return;

	start_aux_cmd(strobe_commands[index], true, IDM_STROBE, AWU_15MS);
}
#else
/** @brief Stub when strobe support is disabled. */
void start_aux_strobe(uint8_t index)
{
	(void)index;
}
#endif

/**
 * @brief Пошаговая обработка одной AUX-команды (AWU-режим)
 * @param aux   — указатель на команду
 * @param ubits — указатель на структуру состояния
 *
 * Каждый вызов обрабатывает ОДИН переход автомата:
 *   PER_INIT    → загрузка count → PER_FIRST
 *   PER_FIRST   → яркость0, AWU per0 → PER_SECOND (MCU спит per0)
 *   PER_SECOND  → яркость1, AWU per1 → PER_END (MCU спит per1)
 *   PER_END     → счётчик, яркость0, AWU per0 → PER_SECOND (без паузы)
 *
 * Вызывается из handle_aux() после пробуждения MCU.
 * AWU таймер задаёт длительность сна между вызовами.
 */
void handle_aux_cmd(union AUX_Cmd_u *aux, AUX_Ctr_t *ubits) {
	if (!ubits->enable) return;

	LED_Bits_t device = (LED_Bits_t)aux->aux.device;

	//EE_PRINTF("[AUX] cmd dev=%d per=%d cnt=%d\r\n", device, ubits->period, ubits->work_count);

	/* Инициализация: загружаем количество повторов */
	if (ubits->period == PER_INIT) {
		ubits->work_count = aux->aux.count;
		ubits->period = PER_FIRST;
	}

	/* Первый полупериод: яркость0, AWU per0 */
	if (ubits->period == PER_FIRST) {
		if (aux->aux.brightness0 != ubits->current_brightness) {
			SetBrightness(device, aux->aux.brightness0);
			ubits->current_brightness = aux->aux.brightness0;
		}
		//EE_PRINTF("[AUX] P0 psc=%d win=%d br=%d\r\n", aux->aux.psc0, aux->aux.win0, aux->aux.brightness0);
		AWU_set_raw(aux->aux.psc0, aux->aux.win0);
		ubits->period = PER_SECOND;
	}
	/* Второй полупериод: яркость1, AWU per1 */
	else if (ubits->period == PER_SECOND) {
		if (aux->aux.brightness1 != ubits->current_brightness) {
			SetBrightness(device, aux->aux.brightness1);
			ubits->current_brightness = aux->aux.brightness1;
		}
		//EE_PRINTF("[AUX] P1 psc=%d win=%d br=%d\r\n", aux->aux.psc1, aux->aux.win1, aux->aux.brightness1);
		AWU_set_raw(aux->aux.psc1, aux->aux.win1);
		ubits->period = PER_END;
	}
	/* Конец полного цикла: обработка счётчика + запуск следующего цикла */
	else if (ubits->period == PER_END) {

		/* Обработка зацикливания и счётчика повторов */
		if (!aux->aux.cycle) {
			if (ubits->work_count > 0) ubits->work_count--;
			if (ubits->work_count == 0) {
				ubits->enable = false;
				ubits->period = PER_NEXT;
				if (device & HBL_LED) {
					/* A finite HBL command may end with brightness1 != 0.
					 * Its terminal state must still be dark. */
					SetBrightness(HBL_LED, 0u);
				}
				smart_off_leds(device);
				return;
			}
		}

		/* Запуск следующего цикла: яркость0 + AWU per0 (без паузы) */
		if (aux->aux.brightness0 != ubits->current_brightness) {
			SetBrightness(device, aux->aux.brightness0);
			ubits->current_brightness = aux->aux.brightness0;
		}
		AWU_set_raw(aux->aux.psc0, aux->aux.win0);
		ubits->period = PER_SECOND;
	}
}

/**
 * @brief Пошаговая обработка массива команд
 * @param pArray — указатель на массив команд в Flash
 * @param ctr    — указатель на структуру состояния канала
 * @return указатель на текущую команду, или NULL если AC_STOP
 *
 * Если текущий элемент — системная команда (is_cmd == 0):
 *   AC_STOP        → возвращает NULL (handle_aux отключит массив)
 *   AC_REP_ATSTART → сбрасывает индекс на начало массива
 *   AC_REP_ATSTEPS → устанавливает индекс на заданный шаг
 *
 * Если текущий элемент — команда для светодиода:
 *   Обрабатывается только если period == PER_NEXT
 *   (т.е. предыдущая команда полностью завершена).
 *   Увеличивает индекс, загружает следующую команду.
 */
union AUX_Cmd_u *handle_aux_array(const uint32_t pArray[], AUX_Ctr_t *ctr) {

	union AUX_Cmd_u *p = (union AUX_Cmd_u*)pArray;

	/* Если предыдущая LED-команда завершена (PER_NEXT) — переходим к
	 * следующему элементу массива. Иначе продолжаем обрабатывать текущий
	 * (он может быть системной командой, до которой не дошли ранее). */
	if (ctr->period == PER_NEXT) {
		ctr->period = PER_INIT;
		ctr->arr_index++;
	}

	p += ctr->arr_index;

	/* Обработка системных команд в цикле (симметрично для первой и
	 * последующих позиций). guard ограничивает число итераций размером
	 * поля arr_index (4 бита = 0..15) — защита от взаимной зацикленности
	 * команд AC_REP_*. */
	uint8_t guard = 16;
	while (p->cmd.is_cmd == 0 && guard--) {
		switch ((Array_Cmd_t)p->cmd.command) {
		case AC_STOP:
			ctr->enable = false;
			return NULL;
		case AC_REP_ATSTART:
			ctr->arr_index = 0;
			p = (union AUX_Cmd_u*)pArray;
			break;
		case AC_REP_ATSTEPS:
			ctr->arr_index = p->cmd.steps;
			p = (union AUX_Cmd_u*)pArray + ctr->arr_index;
			break;
		default:
			ctr->enable = false;
			return NULL;
		}
	}

	/* Защита: после 16 итераций всё ещё системная команда — зависание
	 * (AC_REP_* ссылаются друг на друга). Прерываем во избежание краша. */
	if (p->cmd.is_cmd == 0) {
		ctr->enable = false;
		return NULL;
	}

	ctr->enable = true;
	return p;
}

/**
 * @brief Главная функция обработки AUX-светодиодов
 *
 * Обрабатывает ОДИН переход автомата за вызов:
 *   - Загружает команду из массива (если массив активен)
 *   - Вызывает handle_aux_cmd (один переход: PER_FIRST, PER_SECOND, или PER_END)
 *   - Если команда завершилась (PER_NEXT) — НЕ загружает следующую
 *     (следующая загрузится на следующем вызове handle_aux)
 *
 * После return main loop вызывает AWU_Sleep_Now() — MCU спит ровно столько,
 * сколько задано в текущем полупериоде.
 *
 * @return true если обработчик активен (aux_ctl.enable == true)
 */
bool handle_aux(void) {
	AWU_OFF();

	if (EnableArrayProcessing) {
		p_aux = handle_aux_array(current_buf, &aux_ctl);
		if (p_aux == NULL) EnableArrayProcessing = false;
		else handle_aux_cmd(p_aux, &aux_ctl);
	} else if (p_aux) {
		handle_aux_cmd(p_aux, &aux_ctl);
	}

	bool pending = EnableArrayProcessing || aux_ctl.enable;
	if (pending) AWU_ON();
	return pending;
}

/**
 * @brief Запуск режима маяка (beacon)
 *
 * Формирует команду из настроек EEPROM (aux_flags):
 *   - beacon_on:   включение/выключение (0=выкл, 1=вкл)
 *   - beacon_mode: частота моргания (BCON_1S..BCON_8S — пауза OFF)
 *   - beacon_type: цвет светодиода (RED_LED=1, GRN_LED=2, HBL_LED=4)
 *
 * ON всегда 50мс, OFF определяется beacon_mode.
 */
void start_aux_beacon(void) {
	if (!EE.aux.beacon_on) return;
	if (is_aux_active()) return;

	/* Параметры OFF-полупериода по beacon_mode:
	 * BCON_1S: psc=2048, win=63 → (2048×63)/128000 ≈ 1.0с
	 * BCON_2S: psc=10240, win=25 → (10240×25)/128000 = 2.0с
	 * BCON_4S: psc=10240, win=50 → (10240×50)/128000 = 4.0с
	 * BCON_8S: psc=61440, win=17 → (61440×17)/128000 ≈ 8.2с */
	static const uint8_t bcon_psc1[] = {AWU_PSC_2048, AWU_PSC_10240, AWU_PSC_10240, AWU_PSC_61440};
	static const uint8_t bcon_win1[] = {63,           25,            50,            17};

	uint8_t mode = EE.aux.beacon_mode & 0x03;

	uint32_t cmd = MAKE_CMD(EE.aux.beacon_type, 1, 0,
		AWU_PSC_256, 25, bcon_psc1[mode], bcon_win1[mode],
		1, 0);

	start_aux_cmd(cmd, true, IDM_BEACON, AWU_120MS);
}

#ifdef USE_EX_AUX
/**
 * @brief Запуск индикации предупреждения о напряжении
 * @param ex_mode — скорость моргания (VWARNING_FAST=2с, MEDIUM=4с, SLOW=8с)
 *
 * Зацикленная (cycle=1) команда: одна вспышка RED_LED за период.
 * ON-полупериод — короткая вспышка, OFF — пауза до следующей вспышки.
 * Сумма ON+OFF ≈ период предупреждения (2/4/8 с).
 * Яркость0=ярко, Яркость1=выкл.
 */
void start_exaux_voltage(WARNING_Fr_t ex_mode) {
	uint32_t cmd = 0;
	switch (ex_mode) {
	case VWARNING_FAST:   /* ON 100мс, OFF 1900мс → период ~2с */
		cmd = MAKE_CMD_MS(RED_LED, 1, 0, 100, 1900, 1, 0);
		break;
	case VWARNING_MEDIUM: /* ON 200мс, OFF 3800мс → период ~4с */
		cmd = MAKE_CMD_MS(RED_LED, 1, 0, 200, 3800, 1, 0);
		break;
	case VWARNING_SLOW:   /* ON 300мс, OFF 7700мс → период ~8с */
		cmd = MAKE_CMD_MS(RED_LED, 1, 0, 300, 7700, 1, 0);
		break;
	default:
		break;
	}
	start_aux_cmd(cmd, true, IDM_VWARNING, AWU_15MS);
}
#endif

/**
 * @brief Остановка команды с проверкой режима
 * @param mode — режим индикации для проверки
 *
 * Если обработчик активен и работает в указанном режиме — останавливает его.
 */
void stop_aux_mode(IDM_Type_t mode) {
	if (is_aux_mode(mode))
		off_aux_cmd(false);
}

/** @brief Stop beacon mode if active. */
void stop_aux_beacon(bool stop_lsi) {
	if (is_aux_mode(IDM_BEACON))
		off_aux_cmd(stop_lsi);
}
/** @brief Stop voltage warning mode if active. */
void stop_aux_voltage(bool stop_lsi) {
	if (is_aux_mode(IDM_VWARNING))
		off_aux_cmd(stop_lsi);
}
/** @brief Stop strobe mode if active. */
void stop_aux_strobe(bool stop_lsi) {
	#ifdef USE_STROBE
	if (is_aux_mode(IDM_STROBE))
		off_aux_cmd(stop_lsi);
	#else
	(void)stop_lsi;
	#endif
}

/**
 * @brief Индикация уровня заряда батареи
 *
 * Останавливает маяк, затем запускает set_aux_count()
 * с количеством вспышек, соответствующих уровню заряда.
 */
void slot_batt_ind(void) {
#ifdef USE_BATTIND
	stop_aux_beacon(false);
	uint8_t flashes = bat_get_flashes();
	//EE_PRINTF("[AUX] slot_batt_ind flashes=%u\r\n", (unsigned)flashes);
	uint32_t cmd = set_aux_count(flashes);
	start_aux_cmd(cmd, true, IDM_OTHER, AWU_120MS);
#endif
}

/**
 * @brief Включение (одна зелёная вспышка)
 */
void aux_on(void) {
	//EE_PRINTF("[AUX] info green\r\n");
	start_aux_cmd(INFO_GRN, true, IDM_OTHER, AWU_120MS);
}

/**
 * @brief Выключение (одна красная вспышка)
 */
void aux_off(void) {
	//EE_PRINTF("[AUX] info red\r\n");
	start_aux_cmd(INFO_RED, true, IDM_OTHER, AWU_120MS);
}

/**
 * @brief Индикация числа (зелёные вспышки или одна красная)
 * @param count — количество (0 = одна красная, иначе count зелёных)
 */
void aux_count(uint8_t count) {
	//EE_PRINTF("[AUX] count=%u\r\n", (unsigned)count);

	uint32_t cmd;
	uint8_t flashes;

	if (count) {
		flashes = count > 15 ? 15 : count;
		cmd = MAKE_CMD(GRN_LED, 0, flashes,
		               AWU_PSC_256, 38,
		               AWU_PSC_1024, 28,
		               1, 0);
	} else {
		cmd = MAKE_CMD(RED_LED, 0, 1,
		               AWU_PSC_256, 38,
		               AWU_PSC_1024, 28,
		               1, 0);
	}

	start_aux_cmd(cmd, true, IDM_OTHER, AWU_120MS);
}

/**********************************************************************************************/
#ifdef AUX_DEBUG
/**
 * @brief Отладочный вывод полей команды через UART
 * @param cmd — 32-битная команда
 *
 * Разбивает команду на поля и выводит их значения.
 * Время в миллисекундах = (psc_значение × win) / 128.
 */
void debug_cmd(uint32_t cmd) {
    union AUX_Cmd_u u;
    u.aux_cmd = cmd;

    EE_PRINTF("[AUX] CMD: 0x%08lX\n", cmd);
    EE_PRINTF("  device    = %d\n", u.aux.device);
    EE_PRINTF("  cycle     = %d\n", u.aux.cycle);
    EE_PRINTF("  count     = %d\n", u.aux.count);
    EE_PRINTF("  psc0/win0 = %d/%d (%ld ms)\n", u.aux.psc0, u.aux.win0,
              (long)AWU_PERIOD_MS(u.aux.psc0, u.aux.win0));
    EE_PRINTF("  psc1/win1 = %d/%d (%ld ms)\n", u.aux.psc1, u.aux.win1,
              (long)AWU_PERIOD_MS(u.aux.psc1, u.aux.win1));
    EE_PRINTF("  bright0   = %d\n", u.aux.brightness0);
    EE_PRINTF("  bright1   = %d\n", u.aux.brightness1);
}
#endif /* AUX_DEBUG */
