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
 *   [16:8]  duration0 — длительность 1-го полупериода в тиках AWU (×15мс)
 *   [25:17] duration1 — длительность 2-го полупериода в тиках AWU (×15мс)
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
//#include "eeprom.h"
//#include "soft_start.h"
#include "main.h"
#include "button_gpio.h"

#include "led_gpio.h"
//#include "macros.h"
//#include "adc.h"
#include "wl_flash.h"


extern struct ee_data ee;

uint8_t vwarning = VWARNING_NONE;			// Текущий уровень предупреждения о напряжении
uint8_t vwarning_old = VWARNING_NONE;		// Предыдущий уровень (для отслеживания изменений)

/* Массивы команд для пошаговой обработки (хранятся в Flash)
 * Каждый элемент — 32-битная команда в формате AUX_Cmd_u.
 * Последний элемент массива — системная команда (AC_REP_ATSTART для зацикливания)
 */
const uint32_t buf_red[5]		= {0b101000000000000010001010001, 0b100010100000000010001010001, 0b101000000000000010001000001, 0b100010100000000010001010001, 0x6};
const uint32_t buf_grn[3] 		= {0b101000000000000010001011010, 0b100010100000000010001011010, 0};
const uint32_t buf_sos_red[5]	= {0b10100000010101110001, 0b0100000101000000101000110001, 0b0100000101000001111000110001, 0b0100000101000000101000110001, 1};

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
void start_aux_array(const uint32_t *pArray) {
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
 * Формат команды: device | (count << 4) | duration0 | duration1 | brightness
 */
void set_aux_count(uint8_t count) {
	stop_wakeup();																		// для уверенности

	as_red.acmd.aux.brightness0 = 1;		// яркость 1-го полупериода = включено
	as_red.acmd.aux.brightness1 = 0;		// яркость 2-го полупериода = выключено
	as_red.acmd.aux.duration0 = AWU_500MS;	// длительность включения (500мс)
	as_red.acmd.aux.duration1 = AWU_60MS;	// длительность выключения (60мс)
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
	start_aux_cmd(INFO_RED, AWU_60MS, true, IDM_OTHER);
}

/**
 * @brief Информационная вспышка зелёным
 * Используется при включении (aux_on)
 */
void aux_cmd_info_grn(void) {
	start_aux_cmd(INFO_GRN, AWU_60MS, true, IDM_OTHER);
}

/**
 * @brief Запуск одиночной AUX-команды
 * @param command   — 32-битная команда (биты 0..2 определяют канал)
 * @param delay     — интервал таймера AWU (AWU_15MS, AWU_60MS, AWU_1S и т.д.)
 * @param enable    — true = начать обработку немедленно
 * @param idm_mode  — режим индикации (IDM_BEACON, IDM_VWARNING, IDM_STROBE, IDM_OTHER)
 *
 * Копирует команду в буфер канала (aux_cmd_red/grn/hbl), сбрасывает структуру
 * состояния, устанавливает указатель активной команды (p_red/grn/hbl).
 * Если канал имел активный массив — отключает его (сбрасывает EnableArrayProcessing).
 */
void start_aux_cmd(uint32_t command, awu_tick_t delay, bool enable, IDM_Type_t idm_mode) {
    LED_Bits_t channel = command & 0x07;	// извлекаем канал из младших 3 бит

	#ifdef AUX_DEBUG
	debug_cmd(command);		// выводим отладочную информацию о команде
	#endif

    // Получаем указатели на структуры канала через таблицы
    union AUX_Cmd_u *cmd = get_aux_cmd(channel);    // буфер команды
    AUX_Ctr_t *ctl = get_aux_ctl(channel);          // контроллер состояния
    union AUX_Cmd_u **ptr = get_aux_ptr(channel);   // указатель на p_red/grn/hbl

    if (!cmd || !ctl || !ptr) return;	// невалидный канал — выходим

    // Отключаем пошаговую обработку массива для данного канала,
    // т.к. теперь используется одиночная команда
    if (channel == RED_LED) EnableArrayProcessingRed = false;
    else if (channel == GRN_LED) EnableArrayProcessingGrn = false;
    else if (channel == HBL_LED) EnableArrayProcessingHbl = false;

    cmd->aux_cmd = command;			// копируем команду в буфер канала

    ctl->enable = enable;			// включаем/отключаем обработку
    ctl->idmode = idm_mode;			// устанавливаем режим индикации
    ctl->period = PER_INIT;			// сбрасываем в начальное состояние
    ctl->current_brightness = 0;	// сбрасываем текущую яркость
    ctl->work_duration = 0;			// сбрасываем оставшуюся длительность
    ctl->work_count = 0;			// сбрасываем счётчик повторов

    *ptr = cmd;						// p_red/grn/hbl = указатель на буфер команды

    // Устанавливаем интервал AWU (если не 15мс — таймер уже может быть настроен)
    if (delay != AWU_15MS) {
        AWU_set_delay(delay);
    }
}

/**
 * @brief Установка яркости светодиода
 * @param device — канал (RED_LED, GRN_LED, HBL_LED)
 * @param brightness — яркость (0=выкл, 1=ярко)
 *
 * Вызывается из handle_half_period() при смене яркости в полупериоде.
 * Если brightness > 0 — включает на полную яркость (on_*_h).
 * Если brightness == 0 — умное выключение с учётом подсветки (smart_off_leds).
 */
static void SetBrightness(LED_Bits_t device, uint8_t brightness) {
	switch (device) {
		case RED_LED:
			if (brightness) on_red_h();		// полная яркость
			else smart_off_leds(RED_LED);	// умное выключение
			break;
		case GRN_LED:
			if (brightness) on_grn_h();
			else smart_off_leds(GRN_LED);
			break;
		case HBL_LED:
			// TODO: Реализовать управление подсветкой
			break;
		default:
			break;
	}
}

/*
 * Формат команды (старые комментарии):
 *  24-31 биты  - step 2. Яркость в шагах (0=выкл, 1=полная, 2=подтяжка)
 *  16-23 биты  - step 1. Яркость в шагах
 *  12-15 биты  - длительность в WDT0 2
 *   8-11 биты  - длительность в WDT0 1
 *   4-7  биты  - количество повторов (0=бесконечно)
 *   3    бит   - зацикливание (1=вечно)
 *   0-2  биты  - канал управления
 */

/**
 * @brief Обработка одного полупериода (включение или выключение)
 * @param aux          — указатель на команду
 * @param ubits        — указатель на структуру состояния
 * @param brightness   — яркость для данного полупериода
 * @param next_duration — длительность следующего полупериода (загружается при переходе)
 * @param next_period  — следующий период (PER_SECOND или PER_END)
 * @return true если полупериод завершён (длительность достигла 0), false если ещё работает
 *
 * Общая логика для PER_FIRST и PER_SECOND:
 *   1. Если яркость изменилась — установить новую яркость
 *   2. Уменьшить счётчик длительности
 *   3. Если длительность == 0 — перейти к следующему периоду
 */
static bool handle_half_period(union AUX_Cmd_u *aux, AUX_Ctr_t *ubits,
								uint8_t brightness, uint16_t next_duration, PER_Step_t next_period) {
	LED_Bits_t device = (LED_Bits_t) aux->aux.device;	// определяем канал из команды

	// Устанавливаем яркость только если она изменилась (оптимизация)
	if (brightness != ubits->current_brightness) {
		SetBrightness(device, brightness);
		ubits->current_brightness = brightness;
	}

	uint16_t duration = (uint16_t)(ubits->work_duration);	// читаем оставшуюся длительность

	if (--duration) {				// уменьшаем и проверяем (постинкремент)
		ubits->work_duration = duration;	// ещё не 0 — сохраняем новую длительность
		return false;						// полупериод продолжается
	}

	// Длительность достигла 0 — переход к следующему периоду
	ubits->period = next_period;		// устанавливаем следующий период
	ubits->work_duration = next_duration;	// загружаем длительность следующего полупериода
	return true;							// полупериод завершён
}

/**
 * @brief Пошаговая обработка одной AUX-команды
 * @param aux   — указатель на команду
 * @param ubits — указатель на структуру состояния
 *
 * Автомат:
 *   PER_INIT    → загрузка duration0, count → переход в PER_FIRST
 *   PER_FIRST   → яркость0, duration0 тиков → переход в PER_SECOND
 *   PER_SECOND  → яркость1, duration1 тиков → переход в PER_END
 *   PER_END     → уменьшение счётчика, переход в PER_FIRST (или PER_NEXT если count==0)
 *
 * Вызывается каждые 15мс из handle_aux().
 * Структура else-if гарантирует что за один вызов выполнится только один период.
 */
void handle_aux_cmd(union AUX_Cmd_u *aux, AUX_Ctr_t *ubits) {

	if (ubits->enable) {

		// Инициализация: загружаем параметры из команды
		if (ubits->period == PER_INIT) {
			ubits->work_duration = aux->aux.duration0;	// загружаем длительность 1-го полупериода
			ubits->work_count = aux->aux.count;			// загружаем количество повторов
			ubits->period = PER_FIRST;					// переходим к первому полупериоду
		}

		// Первый полупериод: яркость0 на duration0 тиков
		if (ubits->period == PER_FIRST) {
			handle_half_period(aux, ubits, aux->aux.brightness0, aux->aux.duration1, PER_SECOND);
		}

		// Второй полупериод: яркость1 на duration1 тиков
		else if (ubits->period == PER_SECOND) {
			handle_half_period(aux, ubits, aux->aux.brightness1, 0, PER_END);
		}

		// Конец полного цикла: обработка счётчика повторов
		else if (ubits->period == PER_END) {

			ubits->work_duration = aux->aux.duration0;	// загружаем duration0 для следующего цикла
			ubits->period = PER_FIRST;					// возвращаемся к первому полупериоду

			uint8_t cycle = (uint8_t) aux->aux.cycle;		// флаг зацикливания
			uint8_t counter = (uint8_t) ubits->work_count;	// текущий счётчик

			if (!cycle) {			// если зацикливание выключено — уменьшаем счётчик
				counter--;
				ubits->work_count = counter;

				if (!counter) {		// счётчик достиг 0 — команда завершена
					ubits->enable = false;		// отключаем обработку
					ubits->period = PER_NEXT;	// сигнализируем массиву: пора загружать следующую команду
				}
			}
		}
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
			ctr->enable = true;					// включаем обработку новой команды
		}
	}
	return p;									// возвращаем указатель на текущую команду
}

/**
 * @brief Главная функция обработки AUX-светодиодов
 *
 * Вызывается каждые ~15мс из main loop.
 * Для каждого канала (RED, GRN, HBL):
 *   1. Если включена пошаговая обработка массива — вызывает handle_aux_array()
 *   2. Если массив вернул NULL (AC_STOP) — отключает флаг массива
 *   3. Если массив вернул команду — вызывает handle_aux_cmd()
 *   4. Если массив не используется, но указатель валиден — вызывает handle_aux_cmd()
 *      (для одиночных команд из start_aux_cmd)
 */
void handle_aux(void) {
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
}

/**
 * @brief Запуск режима маяка (beacon)
 *
 * Формирует команду из настроек EEPROM:
 *   - beacon_type: тип моргания (задаёт duration0/duration1)
 *   - beacon_mode: частота моргания (задаёт задержку между циклами)
 *
 * Команда: RED_LED | cycle=1 | count=0 | duration=beacon_type | delay=beacon_mode+7
 */
void start_aux_beacon(void) {
	uint32_t cmd = 0;
	uint8_t beacon_type = ee.aux.beacon_type;					// тип моргания из EEPROM
	uint16_t beacon_mode = (uint16_t) ee.aux.beacon_mode;		// частота моргания из EEPROM

	// Формируем команду: device=RED(1), cycle=1, count=0, duration0=beacon_type, delay=(beacon_mode+7)<<12
	cmd = (0b00000000000000010000000100001000 | beacon_type | ((beacon_mode + 7) << 12));
	start_aux_cmd(cmd, AWU_1S, true, IDM_BEACON);
}

#ifdef USE_EX_AUX
/**
 * @brief Запуск индикации предупреждения о напряжении
 * @param ex_mode — скорость моргания (VWARNING_FAST=2с, MEDIUM=4с, SLOW=8с)
 *
 * Формирует команду с нужной длительностью для красного канала.
 * Используется при низком напряжении батареи.
 */
void start_exaux_voltage(WARNING_Fr_t ex_mode) {
	uint16_t low16 = 0;
	switch (ex_mode) {
	case VWARNING_FAST:
		low16 = 0b0111000000001001;	// duration0=7, duration1=0, brightness=1 — 2с
		break;
	case VWARNING_MEDIUM:
		low16 = 0b1000000000001011;	// duration0=8, duration1=0, brightness=3 — 4с
		break;
	case VWARNING_SLOW:
		low16 = 0b1001000000001010;	// duration0=9, duration1=0, brightness=2 — 8с
		break;
	}
	start_aux_cmd((0b00000000000000010000000000000000 | low16), AWU_1S, true, IDM_VWARNING);
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
	start_aux_cmd(INFO_GRN, AWU_60MS, true, IDM_OTHER);
}

/**
 * @brief Выключение (одна красная вспышка)
 */
void aux_off(void) {
	start_aux_cmd(INFO_RED, AWU_60MS, true, IDM_OTHER);
}

/**
 * @brief Индикация числа (зелёные вспышки или одна красная)
 * @param count — количество (0 = одна красная, иначе count зелёных)
 *
 * Формат команды:
 *   count > 0: GRN_LED | (count << 4) | duration0=1 | duration1=1 | brightness0=1
 *   count == 0: RED_LED | count=2 | duration0=2 | duration1=0 | brightness0=1,1
 */
void aux_count(uint8_t count) {
	uint32_t cmd;
	if (count)
		cmd = 0b00000000000000010100000100000010 | (count << 4);	// зеленые, если не ноль
	else
		cmd = 0b00000000000000010010001000010001;					// одна красная, если 0
	start_aux_cmd(cmd, AWU_250MS, true, IDM_OTHER);
}

/**********************************************************************************************/
#ifdef AUX_DEBUG
/**
 * @brief Отладочный вывод полей команды через UART
 * @param cmd — 32-битная команда
 *
 * Разбивает команду на поля и выводит их значения.
 * Время в миллисекундах = duration × 15мс (интервал AWU).
 */
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

