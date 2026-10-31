# Поддержка DAC MCP4725 с выбором при компиляции (+ EEPROM-синхронизация)

## Context

Проект поддерживает только DAC7571 (0x4C). Нужно: поддержка MCP4725, выбор DAC при компиляции, решение проблемы вспышки от EEPROM. Во flash свободно 4 Б (`Proteus32`), 80 Б (`Proteus32-RGB`).

**Проверено по даташиту MCP4725 (DS22039D):**
- **Power-up**: DAC-регистр автоматически загружается из EEPROM → выход появляется сразу при подаче питания. Заводской EEPROM = `0x000`+PD=Normal → новый модуль не вспыхивает; **б/у модуль (Arduino писал ненулевое) вспыхивает** при каждом включении питания DAC, пока EEPROM не обнулён.
- **Fast-write (2 байта, PD=00) байт-в-байт совпадает** с текущей посылкой DAC7571 (`src/dac.c:122-124`) — различие чипов только в I2C-адресе. I2C power-down прошивка не использует (питание режется пином PD0).
- **Команда "Write DAC Register and EEPROM"**: байт `0x60` (C2=0,C1=1,C0=1), далее `[D11..D4][D3..D0<<4]`. Для нуля: `[0x60][0x00][0x00]` — одна транзакция пишет и регистр, и EEPROM. Подтверждено эталонной [Adafruit_MCP4725](https://github.com/adafruit/Adafruit_MCP4725/blob/master/Adafruit_MCP4725.cpp).
- **t_WR = 25 мс тип / 50 мс макс**, во время записи устройство NACK. Срезать питание DAC (PD0) раньше ~50 мс после выдачи записи нельзя — цикл оборвётся и может исказить EEPROM. В буте нужен `wait_ms(64)` до `OFF_POWER()`.
- **Износ EEPROM 1M циклов** → запись в каждом буте допустима (~27 лет при 100 бутах/день). Рантайм (fast-write) EEPROM не трогает.

**Решение по EEPROM (согласовано):** в буте прошивка пишет `[0x60][0x00][0x00]` — после первого включения модуль самоисцеляется навсегда (вкл. сценарий POR при строб-пробуждении через `dac_pwrpin_restore`). Для нехватки ~40–55 Б в `Proteus32-MCP4725` — `-DUSE_LSI_GUARD=0` только в этом env (риск документирован в AGENTS.md как ничтожный); остальные 3 env без жертв.

## Изменения

### 1. `include/proteus_config.h` — два флага
```c
#ifndef USE_DAC_MCP4725
#define USE_DAC_MCP4725 0   /* ЦАП MCP4725 (0x62) вместо DAC7571 (0x4C); сборка: env Proteus32-MCP4725 / Proteus32-RGB-MCP4725 */
#endif
#ifndef USE_DAC_EEPROM_INIT
#define USE_DAC_EEPROM_INIT 0   /* Только с USE_DAC_MCP4725: в буте писать 0 в DAC-регистр+EEPROM (0x60-команда + wait 64 мс > t_WR 50 мс); лечит вспышку б/у модуля при подаче питания */
#endif
#if USE_DAC_EEPROM_INIT && !USE_DAC_MCP4725
#error "USE_DAC_EEPROM_INIT requires USE_DAC_MCP4725=1 (DAC7571 has no EEPROM)"
#endif
```
EE-раскладка и `WL_FORMAT_VERSION` не затрагиваются.

### 2. `include/dac.h` — адрес + нейтральное имя API
- `#include "proteus_config.h"`; заменить `DAC7571_I2C_ADDR` (строка 34):
  ```c
  #if USE_DAC_MCP4725
  #ifndef MCP4725_I2C_ADDR
  #define MCP4725_I2C_ADDR 0x62   /* типовые модули (A0=VDD); переопределение: -DMCP4725_I2C_ADDR=0x6x */
  #endif
  #define DAC_I2C_ADDR MCP4725_I2C_ADDR
  #else
  #define DAC_I2C_ADDR 0x4C
  #endif
  ```
- Переименовать `dac7571_set_voltage()` → `dac_set_voltage()` (строка 70).
- Обновить Doxygen-шапку: оба чипа, совпадение формата fast-write при PD=00, различие адреса, EEPROM-поведение power-up.

### 3. `src/dac.c` — адрес, переименование, EEPROM-синхронизация
- `DAC7571_I2C_ADDR` → `DAC_I2C_ADDR` (строка 128); переименовать функцию (строка 105); обновить шапку файла.
- Новая функция под `#if USE_DAC_MCP4725` (по образцу транзакции `dac_set_voltage`, с тем же восстановлением `i2c_init(0u)` при сбое):
  ```c
  uint8_t dac_eeprom_write_zero(void)
  {
      if (i2c_start() != I2C_OK ||
          i2c_send_addr(DAC_I2C_ADDR, I2C_DIR_TX) != I2C_OK ||
          i2c_write_byte(0x60) != I2C_OK ||   /* C2=0,C1=1,C0=1: DAC-регистр + EEPROM */
          i2c_write_byte(0x00) != I2C_OK ||   /* [D11..D4] = 0 */
          i2c_write_byte(0x00) != I2C_OK ||   /* [D3..D0<<4] = 0 */
          i2c_stop() != I2C_OK)
      {
          i2c_init(0u);
          return I2C_NACK;    /* следующий бут повторит попытку */
      }
      return I2C_OK;
  }
  ```

### 4. Точки вызова
- `src/ui.c:204` и `src/main.c:220` — переименование; комментарий `src/ui.c:101` → «DAC power-on reset».
- Бут в `src/main.c` (строки 216–221):
  ```c
  #if USE_DAC_EEPROM_INIT
      dac_eeprom_write_zero();
      wait_ms(64);    /* t_WR max 50 мс: цикл записи завершится до срезания питания */
  #else
      dac_set_voltage(0);
  #endif
  ```

### 5. `platformio.ini` — два новых env
```ini
[env:Proteus32-MCP4725]
extends = env:Proteus32
build_flags = 
    ${env:Proteus32.build_flags}
    -DUSE_DAC_MCP4725=1
    -DUSE_DAC_EEPROM_INIT=1
    -DUSE_LSI_GUARD=0

[env:Proteus32-RGB-MCP4725]
extends = env:Proteus32-RGB
build_flags = 
    ${env:Proteus32-RGB.build_flags}
    -DUSE_DAC_MCP4725=1
    -DUSE_DAC_EEPROM_INIT=1
```
+ комментарий-блок с обоснованием (fast-write общий; EEPROM-sync и t_WR; `USE_LSI_GUARD=0` только в `Proteus32-MCP4725` из-за 4 Б свободных). `fw_size.py` гейтит каждый env автоматически.

### 6. `tools/gen_compile_db.py` — защита БД clangd
`RGB_ONLY_FLAGS` (строки 44, 117–121) → словарь с проверкой в обе стороны (флаг своего env присутствует; флаги чужих env отсутствуют):
```python
ENV_ONLY_FLAGS = {
    "Proteus32-RGB": ("-DUSE_IND_RGB=1",),
    "Proteus32-MCP4725": ("-DUSE_DAC_MCP4725=1", "-DUSE_DAC_EEPROM_INIT=1", "-DUSE_LSI_GUARD=0"),
    "Proteus32-RGB-MCP4725": ("-DUSE_DAC_MCP4725=1", "-DUSE_DAC_EEPROM_INIT=1"),
}
```
`DEFAULT_ENV` = `Proteus32` остаётся; docstring обновить.

### 7. Документация
- **AGENTS.md**: блок Build & flash — два новых env; Architecture → DAC — «DAC7571/MCP4725 (`USE_DAC_MCP4725`, адрес `DAC_I2C_ADDR`), fast-write формат общий; `USE_DAC_EEPROM_INIT`: бут-запись 0 в регистр+EEPROM (0x60) + `wait_ms(64)` > t_WR; в `Proteus32-MCP4725` отключён LSI-guard».
- **README.md** (блок строк 134–139): добавить обе команды сборки.

## Примечания
- Комментарии-упоминания DAC7571 в `i2c.h:8`/`i2c.c`/`aux_leds.c:503,556` остаются (утверждения про HS-вход и отсутствие clock stretching верны и для MCP4725).
- Сбой бут-записи (NACK) не критичен: следующий бут повторит (самоисцеление). Окно NACK ~50 мс после записи перекрыто `wait_ms(64)` и тем, что дальнейший I2C-трафик начинается с первого нажатия кнопки.
- Б/у модуль можно вылечить и внешне (Arduino: 3 байта `[0x60][0x00][0x00]`) — как запасной путь в доке.

## Verification
1. `pio run` → `Size: 16124 / 16128 (free 4)`, RAM без изменений (путь DAC7571 — чистый регресс).
2. `pio run -e Proteus32-MCP4725` → гейт проходит (ожидаемо ~16085, свободно ~40); `-e Proteus32-RGB` → 16048; `-e Proteus32-RGB-MCP4725` → гейт проходит. Если какой-то MCP4725-env всё же переполнится — по согласованному фолбэку вернуть этому env `USE_DAC_EEPROM_INIT=0` и отразить в README.
3. `python tools/gen_compile_db.py` → ok; `python tools/gen_compile_db.py --env Proteus32-MCP4725` → ok (проверка флагов в обе стороны).
4. На железе с MCP4725 (`pio run -e Proteus32-MCP4725 -t upload`): (а) записать Arduino-скетчем в EEPROM ненулевое значение → подать питание → после первого бута вспышки нет ни при подаче АКБ, ни при ON_POWER; (б) прогрессия яркости (луна→макс, гамма), строб-профили, AWU-строб; (в) опционально анализатор: адресная фаза `0xC4` (0x62<<1|W), бут `[0x60][0x00][0x00]`, рантайм — 2-байтовый fast-write.
5. Регресс DAC7571: прошивка п.1 на текущем железе — поведение без изменений.

## Источники
- [MCP4725 datasheet DS22039D (Microchip)](https://ww1.microchip.com/downloads/en/devicedoc/22039d.pdf) — power-up из EEPROM, заводской 0x000, fast-write/0x60-команды, t_WR 25/50 мс, endurance 1M.
- [Adafruit_MCP4725.cpp / .h](https://github.com/adafruit/Adafruit_MCP4725/blob/master/Adafruit_MCP4725.cpp) — команды 0x40/0x60, раскладка `[D11..D4][D3..D0<<4]`; [Adafruit tutorial](https://learn.adafruit.com/mcp4725-12-bit-dac-tutorial) — адрес 0x62.