![OpenIPC logo][logo]

# OpenIPC MicroSNANDer

**English** | [Русский](README.ru.md) | [简体中文](README.zh-CN.md)

[![Build](https://github.com/OpenIPC/microsnander/actions/workflows/build.yml/badge.svg)](https://github.com/OpenIPC/microsnander/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/OpenIPC/microsnander/releases/latest)
[![License: GPL-2.0](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)
[![Code size](https://img.shields.io/github/languages/code-size/OpenIPC/microsnander)](https://github.com/OpenIPC/microsnander)
[![Platform](https://img.shields.io/badge/platform-Linux-brightgreen)](#requirements)

MicroSNANDer is a command-line programmer for SPI NOR and SPI NAND flash chips
with the cheap CH341A USB programmer. It reads, writes, erases and verifies the
flash of cameras, routers and other boards, removes the write protection of SPI
NOR chips, and knows 127 SPI NOR and 81 SPI NAND chips.

It is a stripped-down and extended version of
[SNANDer](https://github.com/McMCCRU/SNANDer) 1.7.4 by McMCC: Linux only, without
the EEPROM part in the release build, with the `-u` (unprotect) command and the
flash chips found on OpenIPC hardware added. The release binaries are static and
have no dependencies, so the same file runs on a PC or right on an ARM camera
with the programmer plugged into its USB port.

Changes: [CHANGELOG.md](CHANGELOG.md).

## Quick start

```sh
# 1. Download the static binary (arm: microsnander-linux-arm)
curl -fLo microsnander https://github.com/OpenIPC/microsnander/releases/latest/download/microsnander-linux-x86_64
chmod +x microsnander

# 2. Plug in the CH341A with the chip, detect it
sudo ./microsnander -i

# 3. Back up the whole chip, then write a new image and verify it
sudo ./microsnander -r backup.bin
sudo ./microsnander -w firmware.bin -v
```

## Contents

- [Quick start](#quick-start)
- [Requirements](#requirements)
- [Releases and permanent links](#releases-and-permanent-links)
- [Usage](#usage)
- [Examples](#examples)
- [Supported chips](#supported-chips)
- [Build](#build)
- [Credits](#credits)
- [License](#license)
- [Support](#support)

## Requirements

- A CH341A programmer (USB ID `1a86:5512`) in SPI mode (the jumper on the
  programmer board), with a socket or a clip for the chip. The data lines of the
  common black CH341A board are at 5 V: use a 3.3 V modified board or a level
  adapter for 3.3 V chips, and a 1.8 V adapter for 1.8 V chips.
- Linux on x86_64 or 32-bit ARM (ARMv5TE and later). The release binaries are
  static (musl, libusb built in), nothing to install.
- Access to the USB device: run as root, or allow it to a group with a udev rule:

  ```sh
  echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="1a86", ATTR{idProduct}=="5512", MODE="0660", GROUP="plugdev"' |
    sudo tee /etc/udev/rules.d/60-ch341a.rules
  sudo udevadm control --reload-rules
  ```

## Releases and permanent links

There is one release, [`latest`](https://github.com/OpenIPC/microsnander/releases/latest).
CI rebuilds it from every push to `main` once all builds pass; its title carries the
version (`Microsnander X.Y.Z`), and no versioned releases are kept. The file names
have no version, so these links always give the newest build:

| File                                                                                                                      | For                                                                                          |
|---------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------|
| [`microsnander-linux-x86_64`](https://github.com/OpenIPC/microsnander/releases/latest/download/microsnander-linux-x86_64) | PCs and servers                                                                              |
| [`microsnander-linux-arm`](https://github.com/OpenIPC/microsnander/releases/latest/download/microsnander-linux-arm)       | 32-bit ARM: cameras, Raspberry Pi and other boards (soft-float, runs with or without an FPU) |

Release page: `https://github.com/OpenIPC/microsnander/releases/latest`

## Usage

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

One operation per run: `-i`, `-e`, `-r`, `-w` or `-u`. Without `-a` and `-l` the
operation covers the whole chip. Addresses and lengths are decimal or hex with
`0x`. Erase works on whole blocks: the length must be a multiple of the block size
the chip reports.

SPI NAND chips are read and written with their internal ECC by default: the file
holds the data pages only. With `-d` the ECC is off and every page is read and
written with its OOB (spare) area, for a raw dump.

With `-k` a page that fails ECC on read or fails to program is skipped and the
data goes on in the next page, so everything after it shifts by a page; each
skipped page is printed. An operation that would need a page past the chip end
fails. A page that failed to program is remembered only for `-v` in the same run:
a later `-k` read may read it as data, and the dump is shifted from there on.

## Examples

```sh
# Chip ID, name and size
microsnander -i

# Full backup
microsnander -r full.bin

# 1 MiB from 0x50000 (for example, a kernel partition)
microsnander -r kernel.bin -a 0x50000 -l 0x100000

# Write a full image and verify it
microsnander -w full.bin -v

# Write U-Boot only, to the start of the chip
microsnander -w u-boot.bin -a 0 -v

# Erase 64 KiB at 0x40000 (one block of most SPI NOR chips)
microsnander -e -a 0x40000 -l 0x10000

# SPI NOR chip that refuses to erase or write: remove the protection first
microsnander -u

# SPI NAND: raw dump with the OOB area, ECC off
microsnander -d -r raw.bin
```

## Supported chips

`microsnander -L` prints the list: 127 SPI NOR chips (Winbond, Eon, Fudan
Microelectronics FM25Q, GigaDevice, Macronix, Spansion, XMC, XTX, Zbit, Giantec
GM25Q and others) and 81 SPI NAND chips (Etron, GigaDevice, Heyang, Foresee, ESMT,
Micron, Toshiba/Kioxia, Winbond, Macronix and others).
A chip that is not on the list is reported as not detected; its ID from the
output helps to add it.

## Build

Native build against the system libusb (Debian, Ubuntu):

```sh
sudo apt-get install build-essential libusb-1.0-0-dev
make -C src
```

`make -C src EEPROM_SUPPORT=y` adds the I2C (24Cxx) and Microwire (93Cxx) EEPROM
support of the original SNANDer (`-E`, `-8`, `-f`); it is not in the release
binaries.

The static release binaries are built by `ci/build-musl.sh` (Docker or any Linux
x86_64 host with `curl`, `make` and, for arm, `qemu-user`). It downloads the musl
toolchain and libusb, checks them by SHA256, and builds libusb static without udev:

```sh
ci/build-musl.sh x86_64 dist    # dist/microsnander-linux-x86_64
ci/build-musl.sh arm dist       # dist/microsnander-linux-arm
```

## Credits

MicroSNANDer is based on [SNANDer](https://github.com/McMCCRU/SNANDer)
by McMCC (Serial Nor/nAND/Eeprom programmeR), which builds on the CH341A code of
flashrom and ch341eeprom. The original copyrights are kept in the source files.

## License

[GPL-2.0](LICENSE), as the original SNANDer. See [LICENSE](LICENSE) for the full terms.

## Support

- Questions and bug reports: [GitHub Issues](https://github.com/OpenIPC/microsnander/issues)
- Documentation: [openipc.org](https://openipc.org)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
