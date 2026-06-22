/*
 * aux_leds.c — Модуль управления AUX-светодиодами (красный, зелёный, подсветка)
 *
 * Created: 22.02.2020 17:42:26
 * Modified:15.06.2026
 *  Author: Tamagotchi
 *
 * Поддерживает два режима работы:
 *   1. Одиночная команда (start_aux_cmd) — одно мигание с заданными параметрами
 *   2. Массив команд (start_aux_array) — последовательность команд из массива в Flash
 *
 * Вызов handle_aux() каждые ~15мс (AWU_15MS) обеспечивает пошаговую обработку.
 *
 * Формат 32-битной команды (union AUX_Cmd_u):
 *   [2:0]   device    — канал: 1=RED, 2=GRN, 4=HBL
 *   [3]     cycle     — 0=однократно, 1=зациклено
 *   [7:4]   count     — количество повторов (0..15)
 *   [10:8]  p0.psc    — предделитель 1-го полупериода (0..7 → 128..61440)
 *   [16:11] p0.win    — окно 1-го полупериода (0..63)
 *   [19:17] p1.psc    — предделитель 2-го полупериода
 *   [25:20] p1.win    — окно 2-го полупериода (0..63)
 *   [28:26] brightness0 — яркость 1-го полупериода (0=выкл, 1=ярко)
 *   [31:29] brightness1 — яркость 2-го полупериода (0=выкл, 1=ярко)
 *
 * Массив команд:
 *   Каждый элемент — либо команда для светодиода (is_cmd != 0),
 *   либо системная команда (is_cmd == 0):
 *     AC_STOP        — остановить обработку массива
 *     AC_REP_ATSTART — зациклить с начала массива
 *     AC_REP_ATSTEPS — зациклить с заданного шага
 */


#include "aux_leds.h"
#include "main.h"
#include "button_gpio.h"
#include "led_gpio.h"
#include "wl_flash.h"
#include "ch32v00x.h"
#include "system_ch32v00x.h"

extern struct ee_data ee;

uint8_t vwarning = VWARNING_NONE;			// Текущий уровень предупреждения о напряжении
uint8_t vwarning_old = VWARNING_NONE;		// Предыдущий уровень (для отслеживания изменений)

/* Массивы команд для пошаговой обработки (хранятся в Flash).
 * Формат: MAKE_CMD(device, cycle, count, psc0, win0, psc1, win1, brightness0, brightness1)
 * Время полупериода = (psc_значение × win) / 128 секунд.
 */
const uint32_t buf_red[5] = {
	MAKE_CMD(RED_LED|HBL_LED, 0, 8, AWU_PSC_128, 45, AWU_PSC_4096, 60, 1, 0),   /* ON 45ms, OFF 1920ms × 8 */
	MAKE_CMD(RED_LED|HBL_LED, 0, 4, AWU_PSC_4096, 60, AWU_PSC_128, 45, 1, 0),   /* ON 1920ms, OFF 45ms × 4 */
	MAKE_CMD(RED_LED|HBL_LED, 0, 4, AWU_PSC_128, 45, AWU_PSC_4096, 60, 1, 0),   /* ON 45ms, OFF 1920ms × 4 */
	MAKE_CMD(RED_LED|HBL_LED, 0, 4, AWU_PSC_4096, 60, AWU_PSC_128, 45, 1, 0),   /* ON 1920ms, OFF 45ms × 4 */
	0x6  /* AC_REP_ATSTART */
};
const uint32_t buf_grn[3] = {
	MAKE_CMD(GRN_LED, 0, 1, AWU_PSC_128, 45, AWU_PSC_256, 30, 1, 0),             /* ON 45ms, OFF 60ms */
	MAKE_CMD(GRN_LED, 0, 1, AWU_PSC_256, 30, AWU_PSC_256, 30, 1, 0),             /* ON 60ms, OFF 60ms */
	0  /* AC_REP_ATSTART */
};
const uint32_t buf_sos_red[5] = {
	MAKE_CMD(RED_LED, 0, 0, AWU_PSC_256, 50, AWU_PSC_256, 50, 1, 0),             /* S: ON 100ms, OFF 100ms */
	MAKE_CMD(RED_LED, 0, 0, AWU_PSC_1024, 38, AWU_PSC_256, 50, 1, 0),            /* O: ON 304ms, OFF 100ms */
	MAKE_CMD(RED_LED, 0, 0, AWU_PSC_256, 50, AWU_PSC_256, 50, 1, 0),             /* S: ON 100ms, OFF 100ms */
	MAKE_CMD(RED_LED, 0, 0, AWU_PSC_2048, 44, AWU_PSC_256, 50, 1, 0),            /* Пауза: OFF 704ms */
	0x6  /* AC_REP_ATSTART */
};

/* Тестовый массив: простое мигание RED 512ms ON / 512ms OFF */
const uint32_t buf_test_blink[5] = {
	MAKE_CMD(RED_LED, 0, 2, AWU_PSC_10240, 16, AWU_PSC_10240, 16, 1, 0),  /* ON 512ms, OFF 512ms */
	MAKE_CMD(RED_LED, 0, 2, AWU_PSC_4096, 16, AWU_PSC_4096, 16, 1, 0),  /* ON 512ms, OFF 512ms */
	MAKE_CMD(RED_LED, 0, 2, AWU_PSC_1024, 16, AWU_PSC_1024, 16, 1, 0),  /* ON 512ms, OFF 512ms */
	MAKE_CMD(RED_LED, 0, 2, AWU_PSC_4096, 16, AWU_PSC_4096, 16, 1, 0),  /* ON 512ms, OFF 512ms */
	0x1  /* AC_REP_ATSTART */
};

/* Указатели на текущую команду для каждого канала.
 * Используются в handle_aux() для вызова handle_aux_cmd().
 * Если NULL — канал неактивен.
 */
union AUX_Cmd_u *p_red;
union AUX_Cmd_u *p_grn;
union AUX_Cmd_u *p_hbl;

/* Буферы команд для однократных вызовов (start_aux_cmd).
 * Копируют команду из параметра, чтобы она оставалась доступной
 * пока handle_aux() обрабатывает её пошагово.
 */
union AUX_Cmd_u aux_cmd_red;
union AUX_Cmd_u aux_cmd_grn;
union AUX_Cmd_u aux_cmd_hbl;

/* Структуры состояния (контроллеры) для каждого канала.
 * Содержат: текущий период, яркость, оставшуюся длительность,
 * счётчик повторов, индекс в массиве, режим индикации.
 */
AUX_Ctr_t aux_ctl_red;
AUX_Ctr_t aux_ctl_grn;
AUX_Ctr_t aux_ctl_hbl;

/* Флаги включения пошаговой обработки массивов.
 * true  — handle_aux() вызывает handle_aux_array() для данного канала
 * false — массив не обрабатывается (либо выключен, либо используем одиночную команду)
 */
bool EnableArrayProcessingRed = false;
bool EnableArrayProcessingGrn = false;
bool EnableArrayProcessingHbl = false;

/* Указатели на текущий обрабатываемый массив команд для каждого канала.
 * Нужны, чтобы handle_aux() знал какой массив передавать в handle_aux_array().
 */
const uint32_t *current_buf_red = NULL;
const uint32_t *current_buf_grn = NULL;
const uint32_t *current_buf_hbl = NULL;

/* Таблицы для быстрого доступа к структурам по номеру канала.
 * Индексация: aux_ctl_table[RED_LED] = &aux_ctl_red и т.д.
 * Позволяет избежать switch-case при доступе по каналу.
 */
static AUX_Ctr_t *const aux_ctl_table[] = {
	[RED_LED] = &aux_ctl_red,		// канал 0b001 = 1
	[GRN_LED] = &aux_ctl_grn,		// канал 0b010 = 2
	[HBL_LED] = &aux_ctl_hbl,		// канал 0b100 = 4
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

/**
 * @brief Запуск пошаговой обработки массива команд
 * @param pArray — указатель на массив uint32_t команд в Flash
 *
 * Канал определяется автоматически из бит 0..2 первого элемента массива.
 * Сбрасывает структуру состояния, устанавливает флаг EnableArrayProcessing
 * и запоминает указатель на массив для дальнейшей обработки в handle_aux().
 */
void start_aux_array(const uint32_t *pArray, awu_tick_t delay) {
	LED_Bits_t channel = (LED_Bits_t)(pArray[0] & 0x07);	// извлекаем канал из младших 3 бит

	AUX_Ctr_t *ctl;		// указатель на структуру состояния канала
	bool *flag;			// указатель на флаг EnableArrayProcessing
	const uint32_t **current_buf;	// указатель на текущий буфер
	union AUX_Cmd_u **ptr;			// указатель на p_red/p_grn/p_hbl

	// Определяем все необходимые указатели по номеру канала
	switch (channel) {
	case RED_LED:
		ctl = &aux_ctl_red;
		flag = &EnableArrayProcessingRed;
		current_buf = &current_buf_red;
		ptr = &p_red;
		break;
	case GRN_LED:
		ctl = &aux_ctl_grn;
		flag = &EnableArrayProcessingGrn;
		current_buf = &current_buf_grn;
		ptr = &p_grn;
		break;
	case HBL_LED:
		ctl = &aux_ctl_hbl;
		flag = &EnableArrayProcessingHbl;
		current_buf = &current_buf_hbl;
		ptr = &p_hbl;
		break;
	default:
		return;		// неизвестный канал — выходим
	}

	// Обнуляем структуру состояния (приводим к uint32_t и записываем 0)
	uint32_t *p = (uint32_t*)ctl;
	*p = 0;

	*flag = true;								// включаем пошаговую обработку
	*ptr = (union AUX_Cmd_u*)pArray;			// указатель на текущую команду = начало массива
	*current_buf = pArray;						// запоминаем массив для handle_aux()
	ctl->enable = true;							// включаем обработчик команд

	AWU_set_delay(delay);
	AWU_ON();
}

/**
 * @brief Умное выключение светодиода с учётом режима подсветки
 * @param channel — канал (RED_LED, GRN_LED, HBL_LED)
 *
 * Если подсветка включена (bcklight_type == BCK_TYPE_ON):
 *   - Если цвет подсветки совпадает с каналом — включает слабый режим (on_*_l)
 *   - Иначе — полное выключение (off_*)
 * Если подсветка выключена — полное выключение.
 */
void smart_off_leds(LED_Bits_t channel) {
	if (ee.aux.bcklight_type != BCK_TYPE_ON) {
		if (channel == RED_LED) off_red();
		else if (channel == GRN_LED) off_grn();
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
	case HBL_LED:
		;	// заглушка для подсветки (пока не реализована)
		break;
	default:
		return;
	}
}

/**
 * @brief Получение указателя на структуру состояния канала
 * @param channel — номер канала (RED_LED, GRN_LED, HBL_LED)
 * @return указатель на AUX_Ctr_t или NULL если канал невалиден
 */
static AUX_Ctr_t* get_aux_ctl(LED_Bits_t channel) {
	if (channel < RED_LED || channel > HBL_LED) return NULL;
	return aux_ctl_table[channel];
}

/**
 * @brief Проверка режима индикации канала
 * @param channel — канал
 * @param mode — проверяемый режим (IDM_BEACON, IDM_VWARNING, IDM_STROBE, IDM_OTHER)
 * @return true если канал активен и работает в указанном режиме
 */
bool is_aux_mode(LED_Bits_t channel, IDM_Type_t mode) {
	AUX_Ctr_t *ctl = get_aux_ctl(channel);
	return (ctl && ctl->enable && (ctl->idmode == mode));
}

/**
 * @brief Проверка активности канала
 * @param channel — канал
 * @return true если канал активен (enable == true)
 */
bool is_aux_active(LED_Bits_t channel) {
	AUX_Ctr_t *ctl = get_aux_ctl(channel);
	return (ctl && ctl->enable);
}

// Обёртки для обратной совместимости
bool is_aux_cmd(LED_Bits_t channel) { return is_aux_active(channel); }
bool is_aux_beacon(LED_Bits_t channel) { return is_aux_mode(channel, IDM_BEACON); }
bool is_aux_voltage(LED_Bits_t channel) { return is_aux_mode(channel, IDM_VWARNING); }
bool is_aux_strobe(LED_Bits_t channel) { return is_aux_mode(channel, IDM_STROBE); }
bool is_aux_other(LED_Bits_t channel) { return is_aux_mode(channel, IDM_OTHER); }

/**
 * @brief Получение указателя на буфер команд канала
 * @param channel — канал
 * @return указатель на AUX_Cmd_u или NULL
 */
static union AUX_Cmd_u* get_aux_cmd(LED_Bits_t channel) {
	if (channel < RED_LED || channel > HBL_LED) return NULL;
	return aux_cmd_table[channel];
}

/**
 * @brief Получение указателя на указатель активной команды (p_red/p_grn/p_hbl)
 * @param channel — канал
 * @return указатель на union AUX_Cmd_u* или NULL
 */
static union AUX_Cmd_u** get_aux_ptr(LED_Bits_t channel) {
	if (channel < RED_LED || channel > HBL_LED) return NULL;
	return aux_ptr_table[channel];
}

/**
 * @brief Остановка команды на канале
 * @param channel — канал
 * @param stop_lsi — если true, остановить LSI-генератор (AWU) когда все каналы неактивны
 *
 * Сбрасывает enable, period, idmode; выключает светодиод.
 * Если stop_lsi=true и ни один канал не активен — останавливает AWU.
 */
void off_aux_cmd(LED_Bits_t channel, bool stop_lsi) {
	AUX_Ctr_t *ctl = get_aux_ctl(channel);
	if (!ctl) return;

	ctl->enable = false;		// отключаем обработку
	ctl->period = PER_INIT;		// сбрасываем период
	ctl->idmode = IDM_OTHER;	// сбрасываем режим индикации

	smart_off_leds(channel);	// выключаем светодиод

	if (stop_lsi) {
		// Проверяем, есть ли активные каналы. Если нет — останавливаем AWU.
		if (!is_aux_active(RED_LED) && !is_aux_active(GRN_LED) && !is_aux_active(HBL_LED)) {
			AWU_stop(true);
		}
	}
}

#ifdef USE_BATTIND
/**
 * @brief Формирование команды для индикации уровня заряда батареи
 * @param count — количество вспышек (0 = одна красная, иначе зелёные)
 *
 * Используется в slot_batt_ind() для отображения заряда батареи.
 */
void set_aux_count(uint8_t count) {
	stop_wakeup();																		// для уверенности

	as_red.acmd.aux.brightness0 = 1;		// яркость 1-го полупериода = включено
	as_red.acmd.aux.brightness1 = 0;		// яркость 2-го полупериода = выключено
	as_red.acmd.aux.p0.psc = AWU_PSC_1024;	// предделитель для 500мс
	as_red.acmd.aux.p0.win = 63;			// окно для 500мс (1024×63/128000 = 504ms)
	as_red.acmd.aux.p1.psc = AWU_PSC_256;	// предделитель для 60мс
	as_red.acmd.aux.p1.win = 30;			// окно для 60мс (256×30/128000 = 60ms)
	as_red.acmd.aux.count = count;			// количество повторов
	as_red.acmd.aux.cycle = 0;				// без зацикливания

#ifdef USE_EX_AUX
	// Определяем канал по количеству вспышек
	if (count == 1)
		as_red.acmd.aux.device = RED_BIT;				// 1 вспышка = красный
	else if (count == 2)
		as_red.acmd.aux.device = RED_BIT | GRN_BIT;	// 2 вспышки = красный + зелёный
	else
		as_red.acmd.aux.device = GRN_BIT;				// 3+ вспышки = зелёный
#else
	if(count < 0) as_red.acmd.aux.device = RED_BIT;
	else as_red.acmd.aux.device = GRN_BIT;
	#endif

	if (ee.usr.id_plus_sid) {													// флаг включения дублирования ИДа СИДом
		as_red.acmd.aux.brightness0 |= step_user ? step_user : step_default;//	если дублируем - используем текущий step и сохраняем младший бит для работы ИДа
		as_red.acmd.aux.device |= HBL_BIT;
	}

	as_red.ubits.enable = true;
	AUX_Set_Delay(AWU_15MS);								//включаем сторожевой таймер интервалом delay (WDTO_15MS)
}
#endif

/**
 * @brief Информационная вспышка красным
 * Используется при выключении (aux_off)
 */
void aux_cmd_info_red(void) {
	start_aux_cmd(INFO_RED, true, IDM_OTHER, AWU_120MS);
}

/**
 * @brief Информационная вспышка зелёным
 * Используется при включении (aux_on)
 */
void aux_cmd_info_grn(void) {
	start_aux_cmd(INFO_GRN, true, IDM_OTHER, AWU_120MS);
}

/**
 * @brief Запуск одиночной AUX-команды
 * @param command   — 32-битная команда (тайминг задан в per0/per1)
 * @param enable    — true = начать обработку немедленно
 * @param idm_mode  — режим индикации (IDM_BEACON, IDM_VWARNING, IDM_STROBE, IDM_OTHER)
 *
 * Таймер AWU настраивается в handle_aux_cmd при обработке PER_FIRST.
 */
void start_aux_cmd(uint32_t command, bool enable, IDM_Type_t idm_mode, awu_tick_t delay) {
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
    else if (channel == HBL_LED) EnableArrayProcessingHbl = false;

    cmd->aux_cmd = command;

    ctl->enable = enable;
    ctl->idmode = idm_mode;
    ctl->period = PER_INIT;
    ctl->current_brightness = 0;
    ctl->work_count = 0;

    *ptr = cmd;

	AWU_set_delay(delay);
	AWU_ON();
}

/**
 * @brief Установка яркости светодиода
 * @param device — канал (RED_LED, GRN_LED, HBL_LED)
 * @param brightness — яркость (0=выкл, 1=ярко)
 */
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
			break;
		default:
			break;
	}
}

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

	LED_Bits_t device = (LED_Bits_t) aux->aux.device;

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

	union AUX_Cmd_u *p = (union AUX_Cmd_u*) pArray;	// приводим массив к union

	p += ctr->arr_index;	// сдвигаемся на текущий индекс в массиве

	if (p->cmd.is_cmd == 0) {					// если is_cmd == 0 — это системная команда

		switch ((Array_Cmd_t) p->cmd.command) {
		case AC_STOP:							// остановить выполнение массива
			p = NULL;							// NULL сигнализирует handle_aux() об остановке
			break;
		case AC_REP_ATSTART:					// зациклить с начала массива
			ctr->arr_index = 0;					// сбрасываем индекс на начало
			p = (union AUX_Cmd_u*) pArray;		// указатель тоже на начало
			ctr->period = PER_INIT;				// сбрасываем период для новой команды
			ctr->enable = true;					// включаем обработку
			break;
		case AC_REP_ATSTEPS:					// зациклить с заданного шага
			ctr->arr_index = p->cmd.steps;		// устанавливаем индекс из поля steps
			p = (union AUX_Cmd_u*) pArray;		// указатель на начало массива
			p += ctr->arr_index;				// сдвигаем на нужный шаг
			ctr->period = PER_INIT;				// сбрасываем период
			ctr->enable = true;					// включаем обработку
			break;
		default:
			break;
		}
	} else {									// это команда для светодиода (не системная)
		if (ctr->period == PER_NEXT) {			// пора загружать новую команду
			ctr->period = PER_INIT;				// сбрасываем период для инициализации
			ctr->arr_index++;					// переходим к следующему элементу
			p++;								// сдвигаем указатель
		}
		// Пропускаем системные команды, пока не найдём LED-команду
		//
		// При переходе к следующему элементу (arr_index++) следующий элемент
		// может оказаться системной командой (is_cmd == 0), а не LED-командой.
		// Если вернуть указатель на системную команду — handle_aux_cmd()
		// интерпретирует её как LED-команду и вызовет AWU_set_raw(0, 0),
		// что приведёт к нулевой задержке сна и зависанию MCU.
		//
		// Поэтому обрабатываем системные команды здесь в цикле:
		// сбрасываем индекс/указатель и продолжаем поиск LED-команды,
		// пока не найдём её или не получим команду остановки.
		while (p->cmd.is_cmd == 0) {
			switch ((Array_Cmd_t) p->cmd.command) {
			case AC_STOP:							// Остановить обработку массива
				ctr->enable = false;				// Отключаем канал — обработка завершена
				return NULL;						// NULL → handle_aux отключит EnableArrayProcessing*
			case AC_REP_ATSTART:					// Зациклить с начала массива
				ctr->arr_index = 0;				// Сбрасываем индекс на первый элемент
				p = (union AUX_Cmd_u*) pArray;	// Указатель тоже на начало массива
				break;							// Продолжаем цикл — проверим pArray[0]
			case AC_REP_ATSTEPS:					// Зациклить с заданного шага
				ctr->arr_index = p->cmd.steps;	// Устанавливаем индекс из поля steps команды
				p = (union AUX_Cmd_u*) pArray + p->cmd.steps;	// Сдвигаем указатель на шаг
				break;							// Продолжаем цикл — проверим pArray[steps]
			default:								// Неизвестная системная команда
				ctr->enable = false;				// Безопасная остановка канала
				return NULL;						// Прекращаем обработку массива
			}
		}
		ctr->enable = true;
	}
	return p;									// возвращаем указатель на текущую команду
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
 * @return true если хотя бы один канал активен (aux_ctl.enable == true),
 *         false если все каналы пусты
 */
bool handle_aux(void) {

	//EE_PRINTF("[AUX] Handle AUX\r\n");

	AWU_OFF(); /* Выключаем AWU */

	// === Красный канал ===
	if (EnableArrayProcessingRed) {
		p_red = handle_aux_array(current_buf_red, &aux_ctl_red);	// обрабатываем массив
		if (p_red == NULL) { EnableArrayProcessingRed = false; }	// AC_STOP — отключаем массив
		else { handle_aux_cmd(p_red, &aux_ctl_red); }			// выполняем команду
	} else if (p_red) {
		handle_aux_cmd(p_red, &aux_ctl_red);	// одиночная команда
	}

	// === Зелёный канал ===
	if (EnableArrayProcessingGrn) {
		p_grn = handle_aux_array(current_buf_grn, &aux_ctl_grn);
		if (p_grn == NULL) { EnableArrayProcessingGrn = false; }
		else { handle_aux_cmd(p_grn, &aux_ctl_grn); }
	} else if (p_grn) {
		handle_aux_cmd(p_grn, &aux_ctl_grn);
	}

	// === Подсветка (HBL) ===
	if (EnableArrayProcessingHbl) {
		p_hbl = handle_aux_array(current_buf_hbl, &aux_ctl_hbl);
		if (p_hbl == NULL) { EnableArrayProcessingHbl = false; }
		else { handle_aux_cmd(p_hbl, &aux_ctl_hbl); }
	} else if (p_hbl) {
		handle_aux_cmd(p_hbl, &aux_ctl_hbl);
	}

	/* AWU включаем если есть массивы для обработки ИЛИ одиночные команды.
	 * EnableArrayProcessing* — массив ещё не завершён (нет AC_STOP)
	 * aux_ctl.enable — одиночная команда ещё выполняется */
	bool pending = EnableArrayProcessingRed || EnableArrayProcessingGrn || EnableArrayProcessingHbl
				|| aux_ctl_red.enable || aux_ctl_grn.enable || aux_ctl_hbl.enable;

	if(pending) AWU_ON(); /* Включаем AWU (таймер стартует) */
	
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
	if (!ee.aux.beacon_on) return;

	/* Параметры OFF-полупериода по beacon_mode:
	 * BCON_1S: psc=2048, win=63 → (2048×63)/128000 ≈ 1.0с
	 * BCON_2S: psc=10240, win=25 → (10240×25)/128000 = 2.0с
	 * BCON_4S: psc=10240, win=50 → (10240×50)/128000 = 4.0с
	 * BCON_8S: psc=61440, win=17 → (61440×17)/128000 ≈ 8.2с */
	static const uint8_t bcon_psc1[] = {AWU_PSC_2048, AWU_PSC_10240, AWU_PSC_10240, AWU_PSC_61440};
	static const uint8_t bcon_win1[] = {63,           25,            50,            17};

	uint8_t mode = ee.aux.beacon_mode & 0x03;

	uint32_t cmd = MAKE_CMD(ee.aux.beacon_type, 1, 0,
		AWU_PSC_256, 25, bcon_psc1[mode], bcon_win1[mode],
		1, 0);

	start_aux_cmd(cmd, true, IDM_BEACON, AWU_120MS);
}

#ifdef USE_EX_AUX
/**
 * @brief Запуск индикации предупреждения о напряжении
 * @param ex_mode — скорость моргания (VWARNING_FAST=2с, MEDIUM=4с, SLOW=8с)
 *
 * Формат команды: RED_LED, без зацикливания, 0 повторов, яркость1=ярко.
 * ON/OFF задаются AWU prescaler+window.
 */
void start_exaux_voltage(WARNING_Fr_t ex_mode) {
	uint32_t cmd = 0;
	switch (ex_mode) {
	case VWARNING_FAST:   /* ON 100ms, OFF 1900ms → ~2с */
		cmd = MAKE_CMD(RED_LED, 0, 0, AWU_PSC_256, 50, AWU_PSC_4096, 60, 1, 0);
		break;
	case VWARNING_MEDIUM: /* ON 200ms, OFF 3800ms → ~4с */
		cmd = MAKE_CMD(RED_LED, 0, 0, AWU_PSC_256, 100, AWU_PSC_4096, 60, 3, 0);
		break;
	case VWARNING_SLOW:   /* ON 300ms, OFF 7700ms → ~8с */
		cmd = MAKE_CMD(RED_LED, 0, 0, AWU_PSC_1024, 38, AWU_PSC_61440, 16, 2, 0);
		break;
	}
	start_aux_cmd(cmd, true, IDM_VWARNING);
}
#endif

/**
 * @brief Остановка команды с проверкой режима
 * @param channel — канал
 * @param mode — режим индикации для проверки
 *
 * Если канал активен и работает в указанном режиме — останавливает его.
 */
void stop_aux_mode(LED_Bits_t channel, IDM_Type_t mode) {
	if (is_aux_mode(channel, mode))
		off_aux_cmd(channel, false);
}

// Обёртки для остановки по типу режима
void stop_aux_beacon(LED_Bits_t channel) { stop_aux_mode(channel, IDM_BEACON); }
void stop_aux_voltage(LED_Bits_t channel) { stop_aux_mode(channel, IDM_VWARNING); }
void stop_aux_strobe(LED_Bits_t channel) { stop_aux_mode(channel, IDM_STROBE); }

/**
 * @brief Индикация уровня заряда батареи
 *
 * Останавливает маяк на всех каналах, затем запускает set_aux_count()
 * с количеством вспышек, соответствующих уровню заряда.
 */
void slot_batt_ind(void) {
#ifdef USE_BATTIND
	stop_aux_beacon(RED_LED);
	stop_aux_beacon(GRN_LED);
	stop_aux_beacon(HBL_LED);
	set_aux_count(bat_get_flashes());
#endif
}

/**
 * @brief Включение (одна зелёная вспышка)
 */
void aux_on(void) {
	start_aux_cmd(INFO_GRN, true, IDM_OTHER, AWU_120MS);
}

/**
 * @brief Выключение (одна красная вспышка)
 */
void aux_off(void) {
	start_aux_cmd(INFO_RED, true, IDM_OTHER, AWU_120MS);
}

/**
 * @brief Индикация числа (зелёные вспышки или одна красная)
 * @param count — количество (0 = одна красная, иначе count зелёных)
 */
void aux_count(uint8_t count) {
	uint32_t cmd;
	if (count)
		cmd = 0b00000000000000010100000100000010 | (count << 4);	// зеленые, если не ноль
	else
		cmd = 0b00000000000000010010001000010001;					// одна красная, если 0
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

