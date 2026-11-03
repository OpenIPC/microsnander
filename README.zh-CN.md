![OpenIPC logo][logo]

# OpenIPC MicroSNANDer

[English](README.md) | [Русский](README.ru.md) | **简体中文**

[![Build](https://github.com/OpenIPC/microsnander/actions/workflows/build.yml/badge.svg)](https://github.com/OpenIPC/microsnander/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/OpenIPC/microsnander/releases/latest)
[![License: GPL-2.0](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)
[![Code size](https://img.shields.io/github/languages/code-size/OpenIPC/microsnander)](https://github.com/OpenIPC/microsnander)
[![Platform](https://img.shields.io/badge/platform-Linux-brightgreen)](#系统要求)

MicroSNANDer 是一个命令行编程器，配合廉价的 CH341A USB 编程器使用，用于 SPI NOR 和
SPI NAND 闪存芯片。它可以读取、写入、擦除和校验摄像机、路由器及其他开发板的闪存，
解除 SPI NOR 芯片的写保护，支持 127 款 SPI NOR 和 81 款 SPI NAND 芯片。

它是 McMCC 的 [SNANDer](https://github.com/McMCCRU/SNANDer) 1.7.4 经过精简、增强、深度修改和优化的版本：
仅支持 Linux，发布版本不含 EEPROM 部分，增加了 `-u`（解除保护）命令以及 OpenIPC
硬件上常见的闪存芯片。发布的二进制文件为静态链接、无任何依赖，同一个文件既可以在
PC 上运行，也可以直接在 USB 口插着编程器的 ARM 摄像机上运行。

更新记录：[CHANGELOG.md](CHANGELOG.md)。

## 快速开始

```sh
# 1. 下载静态二进制文件（arm：microsnander-linux-arm）
curl -fLo microsnander https://github.com/OpenIPC/microsnander/releases/latest/download/microsnander-linux-x86_64
chmod +x microsnander

# 2. 插上带芯片的 CH341A，识别芯片
sudo ./microsnander -i

# 3. 备份整片芯片，然后写入新镜像并校验
sudo ./microsnander -r backup.bin
sudo ./microsnander -w firmware.bin -v
```

## 目录

- [快速开始](#快速开始)
- [系统要求](#系统要求)
- [发布与固定链接](#发布与固定链接)
- [用法](#用法)
- [示例](#示例)
- [支持的芯片](#支持的芯片)
- [构建](#构建)
- [致谢](#致谢)
- [许可证](#许可证)
- [支持](#支持)

## 系统要求

- CH341A 编程器（USB ID `1a86:5512`），处于 SPI 模式（编程器板上的跳线），并配有芯片
  座或夹子。常见的黑色 CH341A 板数据线电平为 5 V：3.3 V 芯片请使用改装为 3.3 V 的板
  或电平转换器，1.8 V 芯片请使用 1.8 V 转接板。
- x86_64 或 32 位 ARM（ARMv5TE 及以上）上的 Linux。发布的二进制文件为静态链接（musl，
  内置 libusb），无需安装任何东西。
- USB 设备访问权限：以 root 运行，或用 udev 规则授权给某个用户组：

  ```sh
  echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="1a86", ATTR{idProduct}=="5512", MODE="0660", GROUP="plugdev"' |
    sudo tee /etc/udev/rules.d/60-ch341a.rules
  sudo udevadm control --reload-rules
  ```

## 发布与固定链接

只有一个发布：[`latest`](https://github.com/OpenIPC/microsnander/releases/latest)。
每次推送到 `main` 且所有构建通过后，CI 会重新生成它；标题中带有版本号
（`Microsnander X.Y.Z`），不保留按版本的发布。文件名中不含版本号，因此下面的链接
始终指向最新构建：

| 文件                                                                                                                      | 适用于                                                                 |
|---------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------|
| [`microsnander-linux-x86_64`](https://github.com/OpenIPC/microsnander/releases/latest/download/microsnander-linux-x86_64) | PC 和服务器                                                            |
| [`microsnander-linux-arm`](https://github.com/OpenIPC/microsnander/releases/latest/download/microsnander-linux-arm)       | 32 位 ARM：摄像机、树莓派及其他开发板（soft-float，有无 FPU 均可运行） |

发布页面：`https://github.com/OpenIPC/microsnander/releases/latest`

## 用法

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

每次运行只执行一个操作：`-i`、`-e`、`-r`、`-w` 或 `-u`。不指定 `-a` 和 `-l` 时，
操作覆盖整片芯片。地址和长度可以是十进制，或带 `0x` 的十六进制。擦除按整块进行：
长度必须是芯片报告的块大小的整数倍。

SPI NAND 芯片默认使用片内 ECC 读写：文件中只有数据页。使用 `-d` 时关闭 ECC，每页
连同 OOB（spare）区一起读写，用于原始转储。

使用 `-k` 时，读取时 ECC 失败或写入失败的页会被跳过，数据从下一页继续，因此其后的
内容整体后移一页；每个被跳过的页都会打印出来。写入不会超出 `-a`/`-l` 的范围（不指定
`-l` 时到芯片末尾）：请用 `-l` 指定分区大小，分区内空间不够的写入会失败。需要用到芯片
末尾之后的页的读取也会失败。
写入失败的页只在同一次运行的 `-v` 中被记住：之后用 `-k` 读取时可能把它当作数据读出，
转储从该页起会错位。

## 示例

```sh
# 芯片 ID、型号和容量
microsnander -i

# 完整备份
microsnander -r full.bin

# 从 0x50000 读取 1 MiB（例如内核分区）
microsnander -r kernel.bin -a 0x50000 -l 0x100000

# 写入完整镜像并校验
microsnander -w full.bin -v

# 只把 U-Boot 写到芯片开头
microsnander -w u-boot.bin -a 0 -v

# 擦除 0x40000 处的 64 KiB（大多数 SPI NOR 芯片的一个块）
microsnander -e -a 0x40000 -l 0x10000

# SPI NOR 芯片无法擦除或写入：先解除保护
microsnander -u

# SPI NAND：带 OOB 区的原始转储，关闭 ECC
microsnander -d -r raw.bin
```

## 支持的芯片

`microsnander -L` 会列出支持的芯片：127 款 SPI NOR 芯片（Winbond、Eon、Fudan
Microelectronics FM25Q、GigaDevice、Macronix、Spansion、XMC、XTX、Zbit、Giantec
GM25Q 等）和 81 款 SPI NAND 芯片（Etron、GigaDevice、Heyang、Foresee、ESMT、Micron、
Toshiba/Kioxia、Winbond、Macronix 等）。不在列表中的芯片会显示为未检测到；输出中的
芯片 ID 可用于添加该芯片。

## 构建

使用系统 libusb 的本机构建（Debian、Ubuntu）：

```sh
sudo apt-get install build-essential libusb-1.0-0-dev
make -C src
```

`make -C src EEPROM_SUPPORT=y` 会加入原版 SNANDer 的 I2C（24Cxx）和 Microwire（93Cxx）
EEPROM 支持（`-E`、`-8`、`-f`）；发布的二进制文件中不含该功能。

静态发布文件由 `ci/build-musl.sh` 构建（Docker 或任何带 `curl`、`make` 的 x86_64
Linux，arm 还需要 `qemu-user`）。脚本会下载 musl 工具链和 libusb，按 SHA256 校验，
并以静态方式、不带 udev 构建 libusb：

```sh
ci/build-musl.sh x86_64 dist    # dist/microsnander-linux-x86_64
ci/build-musl.sh arm dist       # dist/microsnander-linux-arm
```

## 致谢

MicroSNANDer 基于 McMCC 的 [SNANDer](https://github.com/McMCCRU/SNANDer)
（Serial Nor/nAND/Eeprom programmeR），而 SNANDer 使用了 flashrom 和 ch341eeprom 的
CH341A 代码。源文件中保留了原始版权声明。

## 许可证

[GPL-2.0](LICENSE)，与原版 SNANDer 相同。完整条款见 [LICENSE](LICENSE)。

## 支持

- 问题与错误报告：[GitHub Issues](https://github.com/OpenIPC/microsnander/issues)
- 文档：[openipc.org](https://openipc.org)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
