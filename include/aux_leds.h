/*
 * aux_leds.h — Модуль управления AUX-светодиодами (красный, зелёный, подсветка)
 *
 * Формат 32-битной команды (union AUX_Cmd_u):
 *   [2:0]   device     — канал: 1=RED, 2=GRN, 4=HBL (можно комбинировать: 3=RED+GRN)
 *   [3]     cycle      — 0=однократно, 1=зациклено (бесконечно)
 *   [7:4]   count      — количество повторов (0..15). При cycle=0 уменьшается
 *   [10:8]  psc0       — индекс предделителя 1-го полупериода (0..7 → 128..61440)
 *   [16:11] win0       — значение окна 1-го полупериода (0..63)
 *   [19:17] psc1       — индекс предделителя 2-го полупериода (0..7)
 *   [25:20] win1       — значение окна 2-го полупериода (0..63)
 *   [28:26] brightness0 — яркость 1-го полупериода (0=выкл, 1=ярко)
 *   [31:29] brightness1 — яркость 2-го полупериода (0=выкл, 1=ярко)
 *
 * Время полупериода: (psc_значение × win) / 128000 секунд
 * Параметры задаются через MAKE_CMD(device, cycle, count, psc0, win0, psc1, win1, brightness0, brightness1)
 */

#ifndef AUX_LEDS_H_
#define AUX_LEDS_H_

#include <stdbool.h>
#include "awu.h"
#include "button_gpio.h"
#include "proteus_config.h"

/* Каналы LED (битовые маски, можно комбинировать: RED|GRN = 3) */
typedef enum {
	RED_LED = 0b001,   /* Красный LED */
	GRN_LED	= 0b010,   /* Зелёный LED */
	HBL_LED	= 0b100    /* Подсветка (backlight) */
} LED_Bits_t;

/* Уровни предупреждения о напряжении (задают частоту моргания) */
typedef enum {
	VWARNING_FAST =	0U,   /* ~2с */
	VWARNING_MEDIUM,          /* ~4с */
	VWARNING_SLOW,            /* ~8с */
	VWARNING_NONE             /* Предупреждение не активно */
} WARNING_Fr_t;

/* Режимы индикации (для отслеживания что сейчас работает) */
typedef enum {
	IDM_OTHER = 0U,    /* Обычное моргание */
	IDM_BEACON,        /* Режим маяка (beacon) */
	IDM_VWARNING,      /* Предупреждение о низком напряжении */
	IDM_STROBE         /* Стробоскоп */
} IDM_Type_t;

/* Цикл полупериодов (finite state machine):
 * PER_INIT → PER_FIRST (ON) → PER_SECOND (OFF) → PER_END → (PER_FIRST или PER_NEXT) */
typedef enum {
	PER_INIT = 0U,  /* Инициализация: загрузка count из команды */
	PER_FIRST,      /* 1-й полупериод: яркость0, MCU спит per0 */
	PER_SECOND,     /* 2-й полупериод: яркость1, MCU спит per1 */
	PER_END,        /* Конец цикла: обработка счётчика, запуск следующего */
	PER_NEXT        /* Команда завершена: нужна загрузка следующей из массива */
} PER_Step_t;

/* Системные команды массива (is_cmd == 0, биты 5..31 == 0).
 * Обрабатываются в handle_aux_array перед возвратом указателя.
 * Если встретилась системная команда — цикл продолжается (break),
 * пока не найдётся LED-команда или AC_STOP. */
typedef enum {
	AC_STOP = 0U,       /* Остановить обработку массива (вернуть NULL) */
	AC_REP_ATSTART,     /* Зациклить с начала массива (arr_index = 0) */
	AC_REP_ATSTEPS,     /* Зациклить с заданного шага (arr_index = steps) */
	AC_RESERVED         /* Зарезервировано */
} Array_Cmd_t;

/* Индексы предделителя AWU (для USE с макросом MAKE_CMD).
 * Записываются в биты psc0/psc1 команды.
 * Фактический делитель = значение таблицы (128..61440).
 * Время = (psc_значение × win) / 128000 секунд */
#define AWU_PSC_128   0   /* LSI/128  = 1 кГц,  win=1 → 1мс */
#define AWU_PSC_256   1   /* LSI/256  = 500 Гц, win=1 → 2мс */
#define AWU_PSC_512   2   /* LSI/512  = 250 Гц, win=1 → 4мс */
#define AWU_PSC_1024  3   /* LSI/1024 = 125 Гц, win=1 → 8мс */
#define AWU_PSC_2048  4   /* LSI/2048 = 62.5Гц, win=1 → 16мс */
#define AWU_PSC_4096  5   /* LSI/4096 = 31.25Гц, win=1 → 32мс */
#define AWU_PSC_10240 6   /* LSI/10240 ≈ 12.5Гц, win=1 → 80мс */
#define AWU_PSC_61440 7   /* LSI/61440 ≈ 2.1Гц, win=1 → 480мс */

/* Вычисление времени полупериода в миллисекундах из индекса предделителя и окна */
#define AWU_PERIOD_MS(psc_idx, win) (((uint32_t)((psc_idx) + 8) * (win) * 1000) / 128000)

/* 32-битная команда LED.
 * Два представления через union:
 *   aux  — команда для светодиода (яркость, тайминг)
 *   cmd  — системная команда массива (AC_STOP, AC_REP_ATSTART, AC_REP_ATSTEPS)
 *   aux_cmd — прямой доступ к 32-битному слову (для записи в Flash)
 *
 * Различие: если биты 5..31 == 0, это системная команда (cmd.command).
 * Иначе — это LED-команда (aux.device, aux.psc0, и т.д.) */
union AUX_Cmd_u {
	struct __attribute__((packed)) {
		uint32_t device      : 3;  /* Биты 0..2:   канал (1=RED, 2=GRN, 4=HBL, комбинации: 3=RED+GRN) */
		uint32_t cycle       : 1;  /* Бит 3:       0=однократно, 1=зациклено (бесконечно) */
		uint32_t count       : 4;  /* Биты 4..7:   количество повторов (0..15). При cycle=0 уменьшается */
		uint32_t psc0        : 3;  /* Биты 8..10:  индекс предделителя AWU для 1-го полупериода (0..7) */
		uint32_t win0        : 6;  /* Биты 11..16: значение окна AWU для 1-го полупериода (0..63) */
		uint32_t psc1        : 3;  /* Биты 17..19: индекс предделителя AWU для 2-го полупериода */
		uint32_t win1        : 6;  /* Биты 20..25: значение окна AWU для 2-го полупериода */
		uint32_t brightness0 : 3;  /* Биты 26..28: яркость 1-го полупериода (0=выкл, 1=ярко) */
		uint32_t brightness1 : 3;  /* Биты 29..31: яркость 2-го полупериода */
	} aux;

	struct __attribute__((packed)) {
		uint32_t command  : 2;   /* Биты 0..1:  системная команда (0=STOP, 1=REP_ATSTART, 2=REP_ATSTEPS) */
		uint32_t steps    : 3;   /* Биты 2..4:  шаг зацикливания (0..7) для REP_ATSTEPS */
		uint32_t is_cmd  : 27;   /* Биты 5..31: если == 0 → системная команда, иначе LED-команда */
	} cmd;

	uint32_t aux_cmd; /* Прямой доступ к 32-битному слову (для записи/чтения в Flash) */
};

/* Compile-time проверки layout */
_Static_assert(sizeof(union AUX_Cmd_u) == 4, "AUX_Cmd_u must be 32 bits");

/* Контрольная структура состояния обработчика AUX (32 бита).
 * Хранит текущий этап автомата, яркость, индекс в массиве, счётчик.
 * Один экземпляр: aux_ctl */
typedef struct {
	uint32_t enable             : 1;  /* Бит 0:      1=обработка активна, 0=выключена */
	uint32_t period             : 3;  /* Биты 1..3:  текущий этап автомата (PER_INIT..PER_NEXT) */
	uint32_t current_brightness : 3;  /* Биты 4..6:  текущая яркость (для оптимизации: менять только при смене) */
	uint32_t idmode             : 2;  /* Биты 7..8:  режим индикации (IDM_OTHER/BEACON/VWARNING/STROBE) */
	uint32_t arr_index          : 4;  /* Биты 9..12: текущий индекс в массиве команд (0..15) */
	uint32_t work_count         : 4;  /* Биты 13..16: оставшиеся повторы команды (0..15) */
	uint32_t reserved           : 15; /* Биты 17..31: резерв (не используется) */
} __attribute__((packed, aligned(4))) AUX_Ctr_t;

typedef enum {
	BCON_1S	= 0U,
	BCON_2S,
	BCON_4S,
	BCON_8S,
} Beacon_Mode_t;

typedef enum {
	BCK_TYPE_OFF = 0U,
	BCK_TYPE_ON
} BCK_Type_t;

typedef enum {
	BCK_COLOR_RED =	0U,
	BCK_COLOR_GRN
} BCK_Color_t;

/* Единый буфер команд для start_aux_cmd() */
extern union AUX_Cmd_u aux_cmd;
/* Единая структура состояния обработчика AUX */
extern AUX_Ctr_t aux_ctl;
/* Указатель на текущую команду (одиночная или из массива) */
extern union AUX_Cmd_u *p_aux;

extern uint8_t vwarning;              /* Текущий уровень предупреждения о напряжении */
extern uint8_t vwarning_old;          /* Предыдущий уровень (для отслеживания изменений) */

extern bool EnableArrayProcessing;    /* true = обработка массива активна */
extern const uint32_t *current_buf;   /* Указатель на текущий массив команд */

/* Массивы команд (хранятся в Flash, const) */
extern const uint32_t buf_red[];      /* Мигание красным+подсветкой */
extern const uint32_t buf_grn[];      /* Мигание зелёным */
extern const uint32_t buf_sos_red[];  /* SOS-паттерн на красном */

/* ===== API функции ===== */

/* Проверка состояния */
bool is_aux_active(void);             /* true если обработчик активен */
bool is_aux_mode(IDM_Type_t mode);    /* true если работает в указанном режиме */

/* Остановка и управление */
void stop_aux_mode(IDM_Type_t mode);  /* Остановка если работает в указанном режиме */
void stop_aux_beacon(bool stop_lsi);  /* Остановка маяка */
void stop_aux_voltage(bool stop_lsi); /* Остановка предупреждения о напряжении */
void stop_aux_strobe(bool stop_lsi);  /* Остановка стробоскопа */
void off_aux_cmd(bool stop_lsi);      /* Полная остановка + выключение всех LED */

/* Запуск команд */
void start_aux_cmd(uint32_t command, bool enable, IDM_Type_t idm_mode, awu_tick_t delay);
void start_aux_array(const uint32_t *pArray, awu_tick_t delay);
void start_aux_beacon(void);          /* Запуск маяка из настроек EEPROM */
uint32_t set_aux_count(uint8_t count);/* Формирование команды вспышек по количеству */
void aux_count(uint8_t count);        /* Индикация числа: 0 → красная, иначе count зелёных */

/* Управление яркостью */
void smart_off_leds(LED_Bits_t channel); /* Умное выключение с учётом подсветки */

/* Главный цикл обработки (вызывается при пробуждении по AWU)
 * Возвращает true если есть активные команды (AWU будет перезапущен),
 * false если все завершены */
bool handle_aux(void);
void handle_aux_cmd(union AUX_Cmd_u *aux, AUX_Ctr_t *ubits);
union AUX_Cmd_u *handle_aux_array(const uint32_t pBuffer[], AUX_Ctr_t *ubits);

#define INFO_RED  MAKE_CMD(RED_LED, 0, 1, AWU_PSC_1024, 56, AWU_PSC_2048, 56, 1, 0)  /* 1 красная ~448мс */
#define INFO_GRN  MAKE_CMD(GRN_LED, 0, 15, AWU_PSC_512, 56, AWU_PSC_4096, 35, 1, 0) /* 15 зелёных */
#define ERRO_RED  MAKE_CMD(RED_LED, 0, 3, AWU_PSC_128, 15, AWU_PSC_256, 45, 1, 0)   /* 3 красных ~15/90мс */

/**
 * @brief Макрос формирования 32-битной AUX-команды
 *
 * Упаковывает параметры моргания LED в 32-битное слово для хранения в Flash.
 * Результат можно использовать в start_aux_cmd() или в массивах команд.
 *
 * Время полупериода = (psc_значение × win) / 128000 секунд
 *   psc=0 (128),  win=1 → 1мс;    win=50 → 50мс;    win=63 → 63мс
 *   psc=5 (4096), win=1 → 32мс;   win=16 → 512мс;   win=63 → 2.06с
 *   psc=7 (61440), win=1 → 480мс; win=16 → 7.68с
 *
 * @param device       канал (RED_LED=1, GRN_LED=2, HBL_LED=4, RED|GRN=3)
 * @param cycle        0=однократно, 1=зациклено (бесконечно)
 * @param count        количество повторов (0..15). При cycle=0 уменьшается
 * @param psc0         индекс предделителя 1-го полупериода (AWU_PSC_128..AWU_PSC_61440)
 * @param win0         значение окна 1-го полупериода (0..63)
 * @param psc1         индекс предделителя 2-го полупериода
 * @param win1         значение окна 2-го полупериода (0..63)
 * @param brightness0  яркость 1-го полупериода (0=выкл, 1=ярко)
 * @param brightness1  яркость 2-го полупериода (0=выкл, 1=ярко)
 */
#define MAKE_CMD(device, cycle, count, psc0, win0, psc1, win1, brightness0, brightness1) \
	((uint32_t)((device) & 0x7)              | \
	 ((uint32_t)((cycle) & 0x1) << 3)        | \
	 ((uint32_t)((count) & 0xF) << 4)        | \
	 ((uint32_t)((psc0) & 0x7) << 8)         | \
	 ((uint32_t)((win0) & 0x3F) << 11)       | \
	 ((uint32_t)((psc1) & 0x7) << 17)        | \
	 ((uint32_t)((win1) & 0x3F) << 20)       | \
	 ((uint32_t)((brightness0) & 0x7) << 26) | \
	 ((uint32_t)((brightness1) & 0x7) << 29))

#endif /* AUX_LEDS_H_ */
