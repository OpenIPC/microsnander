![OpenIPC logo][logo]

# OpenIPC MicroSNANDer

[English](README.md) | **Русский** | [简体中文](README.zh-CN.md)

[![Build](https://github.com/OpenIPC/microsnander/actions/workflows/build.yml/badge.svg)](https://github.com/OpenIPC/microsnander/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/OpenIPC/microsnander/releases/latest)
[![License: GPL-2.0](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)
[![Code size](https://img.shields.io/github/languages/code-size/OpenIPC/microsnander)](https://github.com/OpenIPC/microsnander)
[![Platform](https://img.shields.io/badge/platform-Linux-brightgreen)](#требования)

MicroSNANDer — консольный программатор микросхем флеш-памяти SPI NOR и SPI NAND
для недорогого USB-программатора CH341A. Он читает, записывает, стирает и проверяет
флеш камер, роутеров и других плат, снимает защиту от записи с микросхем SPI NOR и
знает 127 микросхем SPI NOR и 135 микросхем SPI NAND.

Это урезанная, дополненная, глубоко модифицированная и оптимизированная версия
[SNANDer](https://github.com/McMCCRU/SNANDer) 1.7.4 от McMCC: только Linux, без поддержки EEPROM в релизной сборке, с командой
`-u` (снятие защиты) и микросхемами флеш-памяти, которые встречаются в оборудовании
OpenIPC. Релизные файлы статические и без зависимостей, поэтому один и тот же файл
работает и на ПК, и прямо на ARM-камере с программатором в её USB-порту.

Изменения: [CHANGELOG.md](CHANGELOG.md).

## Быстрый старт

```sh
# 1. Скачать статический файл (для arm: microsnander-linux-arm)
curl -fLo microsnander https://github.com/OpenIPC/microsnander/releases/latest/download/microsnander-linux-x86_64
chmod +x microsnander

# 2. Подключить CH341A с микросхемой и определить её
sudo ./microsnander -i

# 3. Сохранить копию всей микросхемы, затем записать новый образ с проверкой
sudo ./microsnander -r backup.bin
sudo ./microsnander -w firmware.bin -v
```

## Содержание

- [Быстрый старт](#быстрый-старт)
- [Требования](#требования)
- [Релизы и постоянные ссылки](#релизы-и-постоянные-ссылки)
- [Использование](#использование)
- [Примеры](#примеры)
- [Поддерживаемые микросхемы](#поддерживаемые-микросхемы)
- [Сборка](#сборка)
- [Благодарности](#благодарности)
- [Лицензия](#лицензия)
- [Поддержка](#поддержка)

## Требования

- Программатор CH341A (USB ID `1a86:5512`) в режиме SPI (перемычка на плате
  программатора), с панелькой или клипсой для микросхемы. Линии данных у распространённой
  чёрной платы CH341A работают на 5 В: для микросхем на 3,3 В нужна доработанная
  плата на 3,3 В или преобразователь уровней, для микросхем на 1,8 В — переходник
  на 1,8 В.
- Linux на x86_64 или 32-битном ARM (ARMv5TE и новее). Релизные файлы статические
  (musl, libusb встроена), ставить ничего не нужно.
- Доступ к USB-устройству: запуск от root или правило udev для группы:

  ```sh
  echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="1a86", ATTR{idProduct}=="5512", MODE="0660", GROUP="plugdev"' |
    sudo tee /etc/udev/rules.d/60-ch341a.rules
  sudo udevadm control --reload-rules
  ```

## Релизы и постоянные ссылки

Релиз один — [`latest`](https://github.com/OpenIPC/microsnander/releases/latest).
CI пересобирает его при каждом push в `main`, когда все сборки прошли; в заголовке
стоит версия (`Microsnander X.Y.Z`), релизов по версиям нет. В именах файлов нет
версии, поэтому эти ссылки всегда ведут на свежую сборку:

| Файл                                                                                                                      | Для чего                                                                                   |
|---------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------|
| [`microsnander-linux-x86_64`](https://github.com/OpenIPC/microsnander/releases/latest/download/microsnander-linux-x86_64) | ПК и серверы                                                                               |
| [`microsnander-linux-arm`](https://github.com/OpenIPC/microsnander/releases/latest/download/microsnander-linux-arm)       | 32-битный ARM: камеры, Raspberry Pi и другие платы (soft-float, работает с FPU и без него) |

Страница релиза: `https://github.com/OpenIPC/microsnander/releases/latest`

## Использование

```
microsnander [options]

 -h             display this message
 -d             disable internal ECC (use read and write page size + OOB size)
 -I             ECC ignore errors (for read test only)
 -k             skip BAD pages (SPI NAND): use the next page instead
 -L             print list support chips
 -i             read the chip ID info
 -e             erase chip (full or use with -a [-l])
 -l <bytes>     manually set length
 -a <address>   manually set address
 -w <filename>  write chip with data from filename
 -r <filename>  read chip and save data to filename
 -u             unprotect chip
 -v             verify after write on chip
```

За один запуск — одна операция: `-i`, `-e`, `-r`, `-w` или `-u`. Без `-a` и `-l`
операция идёт по всей микросхеме. Адреса и длины — десятичные или шестнадцатеричные
с `0x`. Стирание идёт целыми блоками: длина должна быть кратна размеру блока, который
сообщает микросхема.

Микросхемы SPI NAND по умолчанию читаются и пишутся со встроенным ECC: в файле
только страницы данных. С `-d` ECC выключен, и каждая страница читается и пишется
вместе с областью OOB (spare) — для «сырого» дампа.

С `-k` страница, которая не читается из-за ошибки ECC или не записывается,
пропускается, и данные идут дальше со следующей страницы, поэтому всё после неё
сдвигается на страницу; каждая пропущенная страница выводится. Запись не выходит
за `-a`/`-l` (без `-l` — до конца микросхемы): укажите в `-l` размер раздела, и
запись, которой не хватит места в нём, завершится ошибкой. Чтение, которому нужна
страница за концом микросхемы, тоже завершается ошибкой. Страница, которая не
записалась, запоминается только для `-v` в том же запуске: позднее чтение с `-k`
может прочитать её как данные, и дамп с этого места сдвинется.

## Примеры

```sh
# ID микросхемы, название и размер
microsnander -i

# Полная копия
microsnander -r full.bin

# 1 МиБ с адреса 0x50000 (например, раздел ядра)
microsnander -r kernel.bin -a 0x50000 -l 0x100000

# Записать полный образ с проверкой
microsnander -w full.bin -v

# Записать только U-Boot в начало микросхемы
microsnander -w u-boot.bin -a 0 -v

# Стереть 64 КиБ с адреса 0x40000 (один блок у большинства SPI NOR)
microsnander -e -a 0x40000 -l 0x10000

# Микросхема SPI NOR не стирается и не пишется: сначала снять защиту
microsnander -u

# SPI NAND: «сырой» дамп с областью OOB, ECC выключен
microsnander -d -r raw.bin
```

## Поддерживаемые микросхемы

`microsnander -L` выводит список: 127 микросхем SPI NOR (Winbond, Eon, Fudan
Microelectronics FM25Q, GigaDevice, Macronix, Spansion, XMC, XTX, Zbit, Giantec
GM25Q и другие) и 135 микросхем SPI NAND (Etron, GigaDevice, Heyang, Foresee, ESMT, Dosilicon, XTX,
Micron, Toshiba/Kioxia, Winbond, Macronix и другие). Микросхема не из списка
определяется как ненайденная; её ID из вывода поможет её добавить.

## Сборка

Обычная сборка с системной libusb (Debian, Ubuntu):

```sh
sudo apt-get install build-essential libusb-1.0-0-dev
make -C src
```

`make -C src EEPROM_SUPPORT=y` добавляет поддержку EEPROM I2C (24Cxx) и Microwire
(93Cxx) из оригинального SNANDer (`-E`, `-8`, `-f`); в релизных файлах её нет.

Статические релизные файлы собирает `ci/build-musl.sh` (Docker или любой Linux
x86_64 с `curl`, `make` и, для arm, `qemu-user`). Скрипт скачивает тулчейн musl и
libusb, проверяет их по SHA256 и собирает libusb статически, без udev:

```sh
ci/build-musl.sh x86_64 dist    # dist/microsnander-linux-x86_64
ci/build-musl.sh arm dist       # dist/microsnander-linux-arm
```

## Благодарности

MicroSNANDer основан на [SNANDer](https://github.com/McMCCRU/SNANDer) от McMCC
(Serial Nor/nAND/Eeprom programmeR), который, в свою очередь, использует код CH341A
из flashrom и ch341eeprom. Исходные копирайты сохранены в файлах исходников.

## Лицензия

[GPL-2.0](LICENSE), как и у оригинального SNANDer. Полный текст — в [LICENSE](LICENSE).

## Поддержка

- Вопросы и сообщения об ошибках: [GitHub Issues](https://github.com/OpenIPC/microsnander/issues)
- Документация: [openipc.org](https://openipc.org)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
