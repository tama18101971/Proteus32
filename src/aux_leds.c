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
 *   [28:26] brightness0 — modes[] index 1st half-period (or direct-step
 *                         marker for the battery-indicator HBL command)
 *   [31:29] brightness1 — modes[] index 2nd half-period
 *
 * Array command elements:
 *   Each element is either an LED command (AUX_IS_SYS_CMD == false)
 *   or a system command (AUX_IS_SYS_CMD == true):
 *     AC_STOP        — stop array processing
 *     AC_REP_ATSTART — loop from start of array
 *     AC_REP_ATSTEPS — loop from specified step
 *
 * Created: 22.02.2020
 * Modified: 28.08.2026
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

/* HBL battery indication uses a command-field marker and keeps the exact
 * ramping step separately.  The 3-bit command field cannot carry a 9-bit
 * brightness value without rounding small UI1 levels down to an invisible
 * result. */
static uint16_t aux_hbl_step;

/* CMODE_NOACTION is unused by normal AUX brightness commands and is therefore
 * available as the direct-step marker for the battery-indicator HBL phase. */
#define AUX_HBL_DIRECT_STEP CMODE_NOACTION

#if USE_STROBE
typedef enum {
    STROBE_PROFILE_COMMAND = 0,
    STROBE_PROFILE_ARRAY,
} strobe_profile_type_t;

typedef struct {
    strobe_profile_type_t type;
    union {
        uint32_t command;
        const uint32_t *array;
    } value;
} strobe_profile_t;

/* SOS is the former buf_sos_red[] pattern, moved to the main HBL/DAC output.
 * Each pulse uses the same modes[] index as the single-command profiles. */
static const uint32_t strobe_sos[] = {
    /* S: ... */
    MAKE_CMD_MS(HBL_LED, 0, 3, 126, 126, CMODE_TURBO, CMODE_OFF),
    /* O: --- */
    MAKE_CMD_MS(HBL_LED, 0, 3, 376, 126, CMODE_TURBO, CMODE_OFF),
    /* S: ... */
    MAKE_CMD_MS(HBL_LED, 0, 3, 126, 126, CMODE_TURBO, CMODE_OFF),
    /* Pause between SOS repetitions. */
    MAKE_CMD_MS(HBL_LED, 0, 0, 440, 440, CMODE_OFF, CMODE_OFF),
    AC_REP_ATSTART,
};

/* Bike strobe 2: adapted from Caerus III — steady MID base with two
 * TURBO pulses per cycle, looping forever. */
static const uint32_t strobe_bike2[] = {
    /* MID 3 s, then TURBO 30 ms. */
    MAKE_CMD_MS(HBL_LED, 0, 1, 3000, 30, CMODE_MID, CMODE_TURBO),
    /* MID 180 ms, then TURBO 30 ms. */
    MAKE_CMD_MS(HBL_LED, 0, 1, 180, 30, CMODE_MID, CMODE_TURBO),
    AC_REP_ATSTART,
};

/* A profile may be one packed command or an arbitrary command array.  New
 * multi-phase strobes only need an array and one descriptor below. */
static const strobe_profile_t strobe_commands[STROBE_COUNT] = {
    { STROBE_PROFILE_COMMAND, { .command = MAKE_CMD_MS(HBL_LED, 1, 0, 30, 30, CMODE_TURBO, CMODE_OFF) } }, /* 30ms / 30ms */
    { STROBE_PROFILE_COMMAND, { .command = MAKE_CMD_MS(HBL_LED, 1, 0, 60, 2000, CMODE_TURBO, CMODE_MID) } }, /* 60ms / 2s */
    { STROBE_PROFILE_COMMAND, { .command = MAKE_CMD_MS(HBL_LED, 1, 0, 60, 1008, CMODE_MAX, CMODE_OFF) } }, /* 60ms / ~1s */
    { STROBE_PROFILE_COMMAND, { .command = MAKE_CMD_MS(HBL_LED, 1, 0, 60, 4000, CMODE_MAX, CMODE_OFF) } }, /* 60ms / 4s */
    { STROBE_PROFILE_ARRAY,   { .array = strobe_sos } },
    { STROBE_PROFILE_ARRAY,   { .array = strobe_bike2 } }, /* bike strobe 2 */
};
#endif

/**
 * @brief Запуск пошаговой обработки массива команд
 * @param pArray — указатель на массив uint32_t команд в Flash или RAM
 * @param delay  — начальная задержка перед первым полупериодом (awu_tick_t:
 *                 AWU_15MS, AWU_120MS, AWU_250MS, AWU_500MS, AWU_1S и т.д.)
 *                 или AWU_IMMEDIATELY для немедленного запуска первого элемента
 *
 * Сбрасывает структуру состояния, устанавливает флаг EnableArrayProcessing
 * и запоминает указатель на массив для дальнейшей обработки в handle_aux().
 */
static void start_aux_array(const uint32_t *pArray, awu_tick_t delay) {
	off_aux_cmd(false);
	memset(&aux_ctl, 0, sizeof(aux_ctl));

	EnableArrayProcessing = true;
	p_aux = (union AUX_Cmd_u*)pArray;
	current_buf = pArray;
	aux_ctl.enable = true;

	if (delay == AWU_IMMEDIATELY) {
		/* Симметрично start_aux_cmd(): handle_aux() сначала делает AWU_OFF(),
		 * сбрасывая неистекшее окно предыдущей AUX-команды, затем загружает
		 * первый элемент массива и немедленно запускает его ON-полупериод
		 * с корректным psc0/win0. Раньше 0xFF уходил в AWU_set_delay() и
		 * молча отбрасывался проверкой границ — AWU продолжал отсчитывать
		 * старое окно (до 4 с у строба 60ms/4s), и массив стартовал только
		 * после его истечения. */
		(void)handle_aux();
	} else {
		/* AWU-регистры перепрограммируются только при AWUEN=0: перезапуск
		 * поверх ещё активной команды (off_aux_cmd(false) AWU не гасит)
		 * без AWU_OFF() давал бы разовый сбой тайминга следующего
		 * пробуждения. Симметрично handle_aux(). */
		AWU_OFF();
		AWU_set_delay(delay);
		AWU_ON();
	}
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
	uint8_t bck_on = (AUXF_GET_BCKLIGHT_TYPE(EE.aux) == BCK_TYPE_ON);
	if (channel & RED_LED) {
		(bck_on && AUXF_GET_BCKLIGHT_COLOR(EE.aux) == BCK_COLOR_RED) ? on_red_l() : off_red();
	}
	if (channel & GRN_LED) {
		(bck_on && AUXF_GET_BCKLIGHT_COLOR(EE.aux) == BCK_COLOR_GRN) ? on_grn_l() : off_grn();
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
 * @brief Check whether any AUX command currently owns the AWU.
 * @return true while an AUX command or command array is active.
 */
bool aux_is_active(void)
{
	return aux_ctl.enable != 0U;
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
	       (((LED_Bits_t)AUX_DEVICE(p_aux->aux_cmd) & HBL_LED) != 0u);
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
		if (!driver_is_active()) {
			OFF_POWER();
		}
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

static inline uint32_t make_count_cmd(LED_Bits_t device, uint8_t flashes, uint8_t brightness0) {
	return MAKE_CMD(device, 0, flashes, AWU_PSC_512, 27, AWU_PSC_1024, 40, brightness0, 0);
}

#if USE_BATTIND
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
 * Если EE.usr.id_plus_sid == 1, к каналу добавляется HBL_LED
 * (индикация ИД+СИД — индикатор + основной светодиод). Яркость HBL
 * берётся из последнего шага яркости (step_user ? step_user : step_default),
 * чтобы основной светодиод светился на той же яркости, что и фонарик,
 * но не ниже get_stepi(CMODE_MIN): на минимальной яркости фонарика
 * вспышка иначе не видна.
 * Время: ON=152мс, OFF=448мс.
 * @return 32-битная команда для start_aux_cmd()
 */
static uint32_t set_aux_count(uint8_t count) {
	LED_Bits_t device;
	uint8_t flashes;
	uint8_t brightness0 = 1;

	if (count <= 1) {
		device = RED_LED;
		flashes = count ? count : 1;
	} else if (count <= 3) {
		device = RED_LED | GRN_LED;
		flashes = count;
	} else {
		device = GRN_LED;
		flashes = count;
	}

	if (USRF_GET_ID_PLUS_SID(EE.usr))
	{
		device |= HBL_LED;
		uint16_t hbl = step_user ? step_user : step_default;
		/* Фонарик может светить на минимальной (~лунной) яркости — такой
		 * шаг на DAC не читается глазом. Поднимаем вспышку индикации
		 * минимум до CMODE_MIN, чтобы количество миганий было видно. */
		if (hbl < get_stepi(CMODE_MIN))
			hbl = get_stepi(CMODE_MIN);
		aux_hbl_step = hbl;
		brightness0 = AUX_HBL_DIRECT_STEP;
	}

	return make_count_cmd(device, flashes, brightness0);
}
#endif

/**
 * @brief Запуск одиночной AUX-команды
 * @param command   — 32-битная команда
 * @param idm_mode  — режим индикации (IDM_BEACON, IDM_VWARNING, IDM_STROBE, IDM_OTHER)
 * @param delay     — начальная задержка перед первым полупериодом (awu_tick_t:
 *                    AWU_15MS, AWU_250MS, AWU_500MS, AWU_1S и т.д.) или
 *                    AWU_IMMEDIATELY для немедленного запуска первого шага.
 *                    После первого шага AWU переключается на psc0/win0 команды.
 *
 * Таймер AWU настраивается в handle_aux_cmd при обработке PER_FIRST.
 */
static void start_aux_cmd(uint32_t command, IDM_Type_t idm_mode, awu_tick_t delay) {
	off_aux_cmd(false);

	aux_cmd.aux_cmd = command;

	aux_ctl.enable = true;
	aux_ctl.idmode = idm_mode;
	aux_ctl.period = PER_INIT;
	aux_ctl.current_brightness = 0;
	aux_ctl.work_count = 0;

	p_aux = &aux_cmd;

	if (delay == AWU_IMMEDIATELY) {
		/* Skip the initial PER_INIT wake.  Initialise the fields that PER_INIT
		 * normally loads, then execute the first (ON) half-period immediately. */
		aux_ctl.work_count = AUX_COUNT(aux_cmd.aux_cmd);
		aux_ctl.period = PER_FIRST;
		(void)handle_aux();
	} else {
		AWU_OFF(); /* см. комментарий в start_aux_array() */
		AWU_set_delay(delay);
		AWU_ON();
	}
}

/**
 * @brief Установка яркости каналов AUX/строба
 * @param device Битовая маска каналов (RED_LED, GRN_LED, HBL_LED).
 * @param brightness Индекс уровня в modes[] для HBL, 0/1 для GPIO LED;
 *                   CMODE_NOACTION означает aux_hbl_step.
 */
static __attribute__((noinline)) void SetBrightness(LED_Bits_t device, uint8_t brightness)
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
		out_set_stc(brightness == AUX_HBL_DIRECT_STEP ?
				aux_hbl_step : get_stepi(brightness));
	}
}

#if USE_STROBE
/**
 * @brief Запуск стробоскопа через общий AUX command handler.
 * @param index Strobe profile in range 0..STROBE_COUNT-1.
 */
void start_aux_strobe(uint8_t index)
{
	const strobe_profile_t *profile;

	if (index >= STROBE_COUNT)
		return;

	/* Все профили стробов выводятся через DAC7571. Предшествующий
	 * stop_aux_if() срезал питание DAC (PD0): в MODE_STROB
	 * driver_is_active() == false, и off_aux_cmd() вызывает OFF_POWER().
	 * Без восстановления питания первый немедленный импульс нового
	 * строба уходит в обесточенный DAC (NACK) и теряется — первая
	 * видимая вспышка случается лишь через один полный OFF-полупериод
	 * (~1 с у 60ms/1s, ~4 с у 60ms/4s). ON_POWER() идемпотентен для
	 * пути из slot_strobe(), где рельс уже включён on_driver(). */
	ON_POWER();

	profile = &strobe_commands[index];
	if (profile->type == STROBE_PROFILE_COMMAND) {
		start_aux_cmd(profile->value.command, IDM_STROBE, AWU_IMMEDIATELY);
	} else {
		start_aux_array(profile->value.array, AWU_IMMEDIATELY);
		aux_ctl.idmode = IDM_STROBE;
	}
}
#else
/** @brief Stub when strobe support is disabled. */
void start_aux_strobe(uint8_t index)
{
	(void)index;
}
#endif

static void handle_aux_cmd(union AUX_Cmd_u *aux, AUX_Ctr_t *ubits) {
	if (!ubits->enable) return;

	LED_Bits_t device = (LED_Bits_t)AUX_DEVICE(aux->aux_cmd);

	if (ubits->period == PER_INIT) {
		ubits->work_count = AUX_COUNT(aux->aux_cmd);
		ubits->period = PER_FIRST;
	} else if (ubits->period == PER_END) {
		if (!AUX_CYCLE(aux->aux_cmd)) {
			if (ubits->work_count > 0) ubits->work_count--;
			if (ubits->work_count == 0) {
				if (!EnableArrayProcessing)
					ubits->enable = false;
				ubits->period = PER_NEXT;
				if (device & HBL_LED) {
					SetBrightness(HBL_LED, 0u);
					/* Power down only when the handler really goes idle.
					 * An array chains its next element in this same wake:
					 * cutting the DAC supply here makes that element's first
					 * ON half-period write NACK on the unpowered DAC7571,
					 * losing one pulse per group (SOS played as 2 short /
					 * 2 long / 2 short).  Regression since 7ece51e. */
					if (!EnableArrayProcessing && !driver_is_active()) {
						OFF_POWER();
					}
				}
				smart_off_leds(device);
				return;
			}
		}
		ubits->period = PER_FIRST;
	}

	if (ubits->period == PER_FIRST) {
		if (AUX_BR0(aux->aux_cmd) != ubits->current_brightness) {
			SetBrightness(device, AUX_BR0(aux->aux_cmd));
			ubits->current_brightness = AUX_BR0(aux->aux_cmd);
		}
		AWU_set_raw(AUX_PSC0(aux->aux_cmd), AUX_WIN0(aux->aux_cmd));
		ubits->period = PER_SECOND;
	} else if (ubits->period == PER_SECOND) {
		if (AUX_BR1(aux->aux_cmd) != ubits->current_brightness) {
			SetBrightness(device, AUX_BR1(aux->aux_cmd));
			ubits->current_brightness = AUX_BR1(aux->aux_cmd);
		}
		AWU_set_raw(AUX_PSC1(aux->aux_cmd), AUX_WIN1(aux->aux_cmd));
		ubits->period = PER_END;
	}
}

/**
 * @brief Пошаговая обработка массива команд
 * @param pArray — указатель на массив команд в Flash
 * @param ctr    — указатель на структуру состояния канала
 * @return указатель на текущую команду, или NULL если AC_STOP
 *
 * Если текущий элемент — системная команда (AUX_IS_SYS_CMD == true):
 *   AC_STOP        → возвращает NULL (handle_aux отключит массив)
 *   AC_REP_ATSTART → сбрасывает индекс на начало массива
 *   AC_REP_ATSTEPS → устанавливает индекс на заданный шаг
 *
 * Если текущий элемент — команда для светодиода:
 *   Обрабатывается только если period == PER_NEXT
 *   (т.е. предыдущая команда полностью завершена).
 *   Увеличивает индекс, загружает следующую команду.
 */
static union AUX_Cmd_u *handle_aux_array(const uint32_t pArray[], AUX_Ctr_t *ctr) {

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
	while (AUX_IS_SYS_CMD(p->aux_cmd) && guard--) {
		switch (AUX_SYS_CMD_OP(p->aux_cmd)) {
		case AC_STOP:
			ctr->enable = false;
			return NULL;
		case AC_REP_ATSTART:
			ctr->arr_index = 0;
			p = (union AUX_Cmd_u*)pArray;
			break;
		case AC_REP_ATSTEPS:
			ctr->arr_index = AUX_SYS_CMD_STEPS(p->aux_cmd);
			p = (union AUX_Cmd_u*)pArray + ctr->arr_index;
			break;
		default:
			ctr->enable = false;
			return NULL;
		}
	}

	/* Защита: после 16 итераций всё ещё системная команда — зависание
	 * (AC_REP_* ссылаются друг на друга). Прерываем во избежание краша. */
	if (AUX_IS_SYS_CMD(p->aux_cmd)) {
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
 *   - Если команда завершилась (PER_NEXT) — для массива сразу загружает
 *     следующую команду в том же wake, без лишней тёмной паузы
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
		else {
			handle_aux_cmd(p_aux, &aux_ctl);

			/* A finite LED element ends in PER_NEXT.  Load and start the next
			 * array element in this same wake to avoid a dark AWU interval. */
			if (EnableArrayProcessing && aux_ctl.enable &&
			    (aux_ctl.period == PER_NEXT)) {
				p_aux = handle_aux_array(current_buf, &aux_ctl);
				if (p_aux == NULL) EnableArrayProcessing = false;
				else handle_aux_cmd(p_aux, &aux_ctl);
			}
		}
	} else if (p_aux) {
		handle_aux_cmd(p_aux, &aux_ctl);
	}

	bool pending = EnableArrayProcessing || aux_ctl.enable;
	if (pending) AWU_ON();
	return pending;
}

bool beacon_low_voltage = false;
static uint32_t beacon_double_buf[3];

/**
 * @brief Check whether the beacon is at the start of its repetition cycle.
 * @return true on the cycle-initial AWU wake before the first flash.
 */
bool beacon_is_cycle_start(void)
{
	if (!is_aux_mode(IDM_BEACON)) return false;
	if (aux_ctl.period == PER_INIT) return true;
	if (EnableArrayProcessing) {
		return (aux_ctl.period == PER_END) && (aux_ctl.arr_index == 1);
	} else {
		return (aux_ctl.period == PER_END);
	}
}

/**
 * @brief Проверка напряжения батареи в режиме маяка при пробуждении по AWU.
 *
 * Выполняет замер ненагруженного аккумулятора перед вспышкой маяка.
 * - При критическом разряде (< 2.70 В) переходит в необратимый глубокий сон down_forever().
 * - При напряжении < 3.10 В переключает маяк на двойные вспышки.
 * - При напряжении >= 3.20 В возвращает маяк на одиночные вспышки.
 * - Сразу после замера АЦП обесточивается через adc_stop().
 */
void beacon_battery_check(void)
{
	adc_init();
	bool ok = adc_read();
	adc_stop();

	if (!ok) return;

	int32_t eff_vcode = adc_eff_raw_vcode();

	/* 1. Защита от глубокого разряда (< 2.70 В) */
	if (eff_vcode >= (int32_t)MV_LT(2700))
	{
		uint8_t confirmed = 0;
		for (uint8_t i = 0; i < 3; i++)
		{
			adc_init();
			if (adc_read())
			{
				int32_t code = adc_eff_raw_vcode();
				if (code >= (int32_t)MV_LT(2700))
				{
					confirmed++;
				}
			}
			adc_stop();
		}
		if (confirmed >= 2)
		{
			down_forever();
		}
	}

	/* 2. Пороги переключения режима вспышек (гистерезис):
	 *    Vbat < 3.10 В -> двойные вспышки; >= 3.20 В -> одиночные
	 * (одна реализация гистерезиса — adc.c:beacon_low_voltage_update) */
	if (beacon_low_voltage_update())
	{
		off_aux_cmd(false);
		start_aux_beacon();
	}
}

/**
 * @brief Запуск режима маяка (beacon)
 *
 * Формирует команду из настроек EEPROM (aux_flags):
 *   - beacon_on:   включение/выключение (0=выкл, 1=вкл)
 *   - beacon_mode: частота моргания (BCON_1S..BCON_8S — пауза OFF)
 *   - beacon_type: цвет светодиода (RED_LED=1, GRN_LED=2, HBL_LED=4)
 *
 * Одиночная вспышка при нормальном напряжении (>= 3.2 В).
 * Двойная вспышка при пониженном напряжении (< 3.1 В).
 */
void start_aux_beacon(void) {
	if (!AUXF_GET_BEACON_ON(EE.aux)) return;
	if (menu_beacon_mute) return;   /* сессионный мьют во время меню */
	if (aux_is_active()) return;

	/* Параметры OFF-полупериода по beacon_mode:
	 * BCON_1S: psc=2048, win=63 → (2048×63)/128000 ≈ 1.0с
	 * BCON_2S: psc=10240, win=25 → (10240×25)/128000 = 2.0с
	 * BCON_4S: psc=10240, win=50 → (10240×50)/128000 = 4.0с
	 * BCON_8S: psc=61440, win=17 → (61440×17)/128000 ≈ 8.2с */
	static const uint8_t bcon_psc1[] = {AWU_PSC_2048, AWU_PSC_10240, AWU_PSC_10240, AWU_PSC_61440};
	static const uint8_t bcon_win1[] = {63,           25,            50,            17};

	uint8_t mode = AUXF_GET_BEACON_MODE(EE.aux) & 0x03;
	uint8_t br0 = (AUXF_GET_BEACON_TYPE(EE.aux) & HBL_LED) ? CMODE_MIN : 1;
	LED_Bits_t dev = (LED_Bits_t)AUXF_GET_BEACON_TYPE(EE.aux);

	if (beacon_low_voltage) {
		/* Двойная вспышка: 50 мс ON / 150 мс OFF, затем 50 мс ON / период OFF
		 * (150 мс кодируется макросом как 512×38/128000 = 152 мс — ближайшее
		 * значение по округлению AWU_MS_WIN_FOR). */
		beacon_double_buf[0] = MAKE_CMD_MS(dev, 0, 1, 50, 150, br0, 0);
		beacon_double_buf[1] = MAKE_CMD(dev, 0, 1, AWU_PSC_256, 25, bcon_psc1[mode], bcon_win1[mode], br0, 0);
		beacon_double_buf[2] = AC_REP_ATSTART;
		start_aux_array(beacon_double_buf, AWU_120MS);
		aux_ctl.idmode = IDM_BEACON;
	} else {
		uint32_t cmd = MAKE_CMD(dev, 1, 0,
			AWU_PSC_256, 25, bcon_psc1[mode], bcon_win1[mode],
			br0, 0);
		start_aux_cmd(cmd, IDM_BEACON, AWU_120MS);
	}
}

/**
 * @brief Обновить состояние фоновой подсветки по настройкам EE.aux
 */
void aux_refresh_backlight(void) {
	smart_off_leds(RED_LED | GRN_LED);
}

#if USE_EX_AUX
/**
 * @brief Запуск индикации предупреждения о напряжении
 * @param ex_mode — скорость моргания (VWARNING_FAST=2с, MEDIUM=4с, SLOW=8с)
 *
 * Зацикленная (cycle=1) команда: одна вспышка RED_LED за период.
 * ON-полупериод — короткая вспышка, OFF — пауза до следующей вспышки.
 * Сумма ON+OFF ≈ период предупреждения (2/4/8 с).
 * Первая вспышка — через ~1 с (AWU_1S): «тихая пауза» отделяет начало
 * паттерна от предыдущего AUX-сигнала (ошибка, индикация и т.п.),
 * чтобы пользователь не сливал их в одну последовательность.
 *
 */
void start_exaux_voltage(WARNING_Fr_t ex_mode) {
	uint32_t cmd = 0;
	switch (ex_mode) {
	case VWARNING_FAST:   /* ON 100мс, OFF 1900мс → период ~2с */
		cmd = MAKE_CMD_MS(RED_LED, 1, 0, 100, 1900, 1, 0);
		break;
	case VWARNING_MEDIUM: /* ON 100мс, OFF 3900мс → период ~4с */
		cmd = MAKE_CMD_MS(RED_LED, 1, 0, 100, 3900, 1, 0);
		break;
	case VWARNING_SLOW:   /* ON 100мс, OFF 7900мс → период ~8с */
		cmd = MAKE_CMD_MS(RED_LED, 1, 0, 100, 7900, 1, 0);
		break;
	default:
		break;
	}
	start_aux_cmd(cmd, IDM_VWARNING, AWU_1S);
}
#endif

/**
 * @brief Индикация уровня заряда батареи
 *
 * Останавливает маяк, затем запускает set_aux_count()
 * с количеством вспышек, соответствующих уровню заряда.
 */
void slot_batt_ind(void) {
#if USE_BATTIND
	/* ID+SID uses the DAC as an AUX waveform.  Do not start it over the
	 * normal driver output; on_driver() handles the opposite transition. */
    if (driver_is_active() && USRF_GET_ID_PLUS_SID(EE.usr))
		return;
	stop_aux_if(IDM_BEACON, false);
	uint8_t flashes = bat_get_flashes();
	uint32_t cmd = set_aux_count(flashes);
	/* The HBL battery indication can be requested immediately after the
	 * flashlight was switched off, before STANDBY restores the DAC path. */
	if (USRF_GET_ID_PLUS_SID(EE.usr))
		ON_POWER();
	start_aux_cmd(cmd, IDM_OTHER, AWU_120MS);
#endif
}

/**
 * @brief Включение (две зелёные вспышки)
 */
void aux_on(void) {
	start_aux_cmd(INFO_GRN, IDM_OTHER, AWU_60MS);
}

/**
 * @brief Выключение (одна красная вспышка)
 */
void aux_off(void) {
	start_aux_cmd(INFO_RED, IDM_OTHER, AWU_60MS);
}

#if USE_AUX_ERR
/**
 * @brief Индикация ошибки (три короткие красные вспышки)
 * @param delay начальная задержка перед первой вспышкой или AWU_IMMEDIATELY
 *
 * Выполняется только при включённом флаге exaux (UIF_EAUX_ONOFF в EE.uif,
 * меню пользователя); при выключенном — последовательность не запускается.
 */
void aux_err(awu_tick_t delay) {
	/* Прямая проверка маски (без сдвига/расширения UIF_GET_EAUX_ONOFF):
	 * на RV32EC это на 4 байта короче, результат тот же — «бит установлен». */
	if (EE.uif & UIF_EAUX_ONOFF_MASK)
		start_aux_cmd(ERRO_RED, IDM_OTHER, delay);
}

/**
 * @brief aux_err() без задержки (AWU_IMMEDIATELY), форма без аргумента.
 *
 * noinline: единая обёртка с константой дешевле, чем её материализация
 * на каждом immediate-сайте вызова.
 */
__attribute__((noinline)) void aux_err_now(void) {
	aux_err(AWU_IMMEDIATELY);
}

/**
 * @brief Индикация блокировки (одна короткая красная вспышка, DANG_RED)
 *
 * Выполняется только при включённом флаге exaux (UIF_EAUX_ONOFF в EE.uif,
 * меню пользователя); при выключенном — последовательность не запускается.
 */
void aux_dang(void) {
	/* Маска напрямую — см. комментарий в aux_err() (экономия 4 Б). */
	if (EE.uif & UIF_EAUX_ONOFF_MASK)
		start_aux_cmd(DANG_RED, IDM_OTHER, AWU_IMMEDIATELY);
}
#endif

/**
 * @brief Индикация числа (зелёные вспышки или одна красная)
 * @param count — количество (0 = одна красная, иначе count зелёных)
 */
void aux_count(uint8_t count) {
	LED_Bits_t dev = count ? GRN_LED : RED_LED;
	uint8_t flashes = count ? (count > 15 ? 15 : count) : 1;
	start_aux_cmd(make_count_cmd(dev, flashes, 1), IDM_OTHER, AWU_120MS);
}

/** @brief RAM-буфер двухчастной индикации выбора magic-слота (см. aux_magic_slot_info). */
static uint32_t magic_info_buf[3];

/**
 * @brief Индикация выбранного magic-слота: slot_no красных, затем op зелёных.
 *
 * Последовательность собирается в RAM-массиве (прецедент beacon_double_buf)
 * и запускается через AWU-FSM (неблокирующе). Полуволвы make_count_cmd:
 * ON ≈108 мс / OFF ≈320 мс — пауза 320 мс после последней красной отделяет
 * группу номера слота от группы значения.
 *
 * @param slot_no номер слота 1..10 (клампится в 1..15 — поле AUX_COUNT 4 бита)
 * @param op      номер функции в слоте 1..12 (клампится в 1..15)
 */
void aux_magic_slot_info(uint8_t slot_no, uint8_t op)
{
	if (slot_no < 1u)
		slot_no = 1u;
	else if (slot_no > 15u)
		slot_no = 15u;
	if (op < 1u)
		op = 1u;
	else if (op > 15u)
		op = 15u;

	magic_info_buf[0] = make_count_cmd(RED_LED, slot_no, 1);
	magic_info_buf[1] = make_count_cmd(GRN_LED, op, 1);
	/* magic_info_buf[2] не пишется: буфер в BSS (нулевой) и больше нигде не
	 * изменяется, а 0 == MAKE_SYS_CMD(AC_STOP, 0) — терминатор массива. */
	start_aux_array(magic_info_buf, AWU_120MS);
}

/** @brief Команды вспышки-подтверждения magic-режима (Flash, ноль RAM). */
static const uint32_t magic_cue_short = MAKE_CMD_MS(RED_LED | GRN_LED, 0, 2, 150, 150, 1, 0);
static const uint32_t magic_cue_long  = MAKE_CMD_MS(RED_LED | GRN_LED, 0, 1, 500, 500, 1, 0);

/**
 * @brief Оранжевое подтверждение magic-режима (RED+GRN): вход в пикер
 * magic-слотов (2 короткие вспышки) или выход из magic-режима.
 *
 * @param long_pulse false — 2 короткие вспышки по ~150 мс (вход в пикер);
 *                   true — 1 длинная ~500 мс (выход, стиль menu_exit:
 *                   500 мс ON / 500 мс OFF).
 */
__attribute__((noinline)) void aux_magic_cue(bool long_pulse)
{
	start_aux_cmd(long_pulse ? magic_cue_long : magic_cue_short,
		      IDM_OTHER, AWU_IMMEDIATELY);
}

/**
 * @brief Отклонение неверного ввода: три короткие красные вспышки.
 *
 * Безусловно (в отличие от aux_err(), гейтящегося флагом exaux):
 * применяется во всём меню пользователя, magic-режиме и calibration().
 * Без noinline: единственный caller (ui_invalid_selection) — LTO инлайнит
 * сам, отдельная копия только раздувает Flash.
 */
void aux_reject(void)
{
	start_aux_cmd(ERRO_RED, IDM_OTHER, AWU_15MS);
}
