![OpenIPC logo][logo]

# MicroSNANDer

[English](README.md) | [Русский](README.ru.md) | **简体中文**

[![Build](https://github.com/OpenIPC/microsnander/actions/workflows/build.yml/badge.svg)](https://github.com/OpenIPC/microsnander/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/OpenIPC/microsnander/releases/latest)
[![License: GPL-2.0](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)
[![Code size](https://img.shields.io/github/languages/code-size/OpenIPC/microsnander)](https://github.com/OpenIPC/microsnander)
[![Platform](https://img.shields.io/badge/platform-Linux-brightgreen)](#系统要求)

MicroSNANDer 是一个命令行编程器，配合廉价的 CH341A USB 编程器使用，用于 SPI NOR 和
SPI NAND 闪存芯片。它可以读取、写入、擦除和校验摄像机、路由器及其他开发板的闪存，
解除 SPI NOR 芯片的写保护，支持 291 款 SPI NOR 和 135 款 SPI NAND 芯片。

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
- [数据不确定的芯片](#数据不确定的芯片)
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

`microsnander -L` 会列出支持的芯片：291 款 SPI NOR 芯片（Winbond、Eon、Fudan
Microelectronics FM25Q、GigaDevice、Macronix、Spansion、XMC、XTX、Zbit、Giantec
GM25Q、ISSI、Micron、Boya、Puya 等）和 135 款 SPI NAND 芯片（Etron、GigaDevice、Heyang、Foresee、ESMT、Dosilicon、XTX、Micron、
Toshiba/Kioxia、Winbond、Macronix 等）。不在列表中的芯片会显示为未检测到；输出中的
芯片 ID 可用于添加该芯片。

## 数据不确定的芯片

对于下列 SPI NOR 和 SPI NAND 芯片，其他来源给出的参数与 MicroSNANDer 使用的不同。这些芯片都还没有
在硬件上验证过，因此读取、写入，尤其是原始转储（`-d`）的结果可能与这里的描述不同。如果
您有这些芯片，请在 [GitHub Issues](https://github.com/OpenIPC/microsnander/issues)
中报告其工作情况，并附上 `microsnander -i` 的输出和芯片上的完整型号。

| 芯片                                                       | MicroSNANDer 使用                                                                | 其他来源                              |
|------------------------------------------------------------|----------------------------------------------------------------------------------|---------------------------------------|
| TOSHIBA TC58CVG0S3H, TC58CVG1S3H                           | OOB 128 字节                                                                     | OOB 64 字节                           |
| TOSHIBA TC58CVG2S0H, KIOXIA TC58CVG2S0HRAIJ                | OOB 256 字节                                                                     | OOB 128 字节                          |
| MICRON MT29F4G01（ID `2C 36`）                             | 两个 2 Gbit 晶粒，页 2 KB，OOB 128 字节                                          | 单晶粒，页 4 KB，OOB 256 字节         |
| 厂商 ID 为 `C9` 的 HEYANG 芯片                             | 状态位 5:4 为 `10` 表示 ECC 不可纠正；解锁清除位 5:3                             | 位 5:4 为 `11`；解锁位 6:3            |
| DOSILICON DS35Q1GA, DS35Q2GA                               | 状态位 5:4 为 `10` 表示 ECC 不可纠正                                             | 位 6:4 为 `010`                       |
| GIGADEVICE GD5F1GM7UE                                      | OOB 128 字节，dual 读取                                                          | OOB 64 字节，single 读取              |
| ESMT F50D1G41LB(2M)                                        | OOB 64 字节                                                                      | OOB 128 字节                          |
| FORESEE F35SQA001G                                         | OOB 64 字节                                                                      | OOB 128 字节                          |
| FORESEE F35SQA512M                                         | dual 读取                                                                        | single 读取                           |
| XINCUN XCSP1AAWH-NT                                        | 不检查 ECC 状态：不可纠正的页读取时不报错                                        | 状态编码未知                          |
| UNIM UM19C0HISW                                            | 暂不支持                                                                         | spare 区计入页和块大小的几何参数      |
| ES25P10–ES25P32、ES25M40A–ES25M16A（SPI NOR）              | 按 ID 中的容量字节确定大小（ES25P10：1 Mbit）                                    | 两倍大小                              |
| ISSI `9D 60 1A`、`9D 70 1A`（SPI NOR）                     | IS25LP512M、IS25WP512M，512 Mbit                                                 | 命名为 IS25LP256D、IS25WP256D         |
| PCT25VF020B–PCT25VF032B、PCT26VF016、PCT26VF032（SPI NOR） | AAI 字编程（SST25 类型）、ULBPR 解锁和 4 KB 擦除（SST26 类型）；在 CH341A 上较慢 | 作为支持 256 字节页编程的普通芯片列出 |
| PCT25VF010A（SPI NOR）                                     | 不支持：没有 JEDEC READ ID                                                       | 列有 JEDEC ID                         |

OOB 大小只影响使用 `-d` 的原始转储（每页连同 spare 区一起读取）；使用片内 ECC 的读写只
涉及数据区。原始写入只接受完整的“数据 + OOB”页，因此 1.11.1 之前（使用旧 OOB 大小）
制作的上述 Toshiba/Kioxia 芯片原始转储会被拒绝：请先把每页用 `0xFF` 补齐到新的 OOB 大小。

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
