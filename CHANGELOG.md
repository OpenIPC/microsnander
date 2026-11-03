# Changelog

The version lives in `_VER` in `src/main.c`. Binaries are published only as the
[`latest`](https://github.com/OpenIPC/microsnander/releases/latest) release, rebuilt
from every push to `main`; this file is the change history.

## 1.11.0 — 2026-11-03

- 165 more SPI NOR chips, 292 in all: GD25Q20C, GD25Q40C, GD25Q80C, GD25LQ80C,
  GD25WD80C, GD25WQ80E, GD25LQ16C, GD25WQ16E, GD25LQ32E, GD25WQ32E, GD25LQ64E,
  GD25LQ128D, GD25F256F, GD25LB256E, MX25L4005A, MX25L8005M, MX25U1635F, MX25U3235F,
  MX25U6435F, MX25U12835F, MX25U25643G, MX25U51245G, S25FL512S, EN25XQ128A, EN25Q256,
  W25Q32JWIM, W25Q64JVIM, W25Q64JWIM, W25Q256JW, W25Q256JWIM, W25Q512JV, W25Q512JVIM,
  W25Q512NW, W25Q512NWIM, M25P05, M25P10, M25P20, M25P40, M25P80, M25P32, M25P64,
  N25Q016A, N25Q032A, N25Q064A, N25Q128A, MT25QU256AB, MT25QU512AB, XM25QH10B,
  XM25QH20B, XM25QU41B, XM25QH40B, XM25QU80B, XM25QH80B, XM25QU16B, XM25QH16C,
  XM25QW16C, XM25QW32C, XM25LU32C, XM25LU64C, XM25QW64C, XM25LU128C, XM25QW128C,
  XM25QH256C, XM25QU256C, XM25QW256C, XM25QH512C, XM25QU512C, XM25QW512C, ZB25LQ16,
  ZB25LQ32, ZB25LQ64, ZB25LQ128, BY25D05AS, BY25D10AS, BY25D20AS, BY25D40AS,
  BY25Q40BL, BY25Q40BL, BY25Q80BS, BY25Q16BL, BY25Q32AL, BY25Q64AL, BY25Q128EL,
  BY25Q256ES, XT25F04D, XT25F08B, XT25F08D, XT25Q16D, XT25Q64D, XT25F128D, IS25LQ010,
  IS25LQ020, IS25WP040D, IS25LP080D, IS25WP080D, IS25WP016D, IS25WP032D, IS25WP064D,
  IS25WP128F, IS25LP256D, IS25WP256D, IS25LP512M, IS25WP512M, FM25W04, FM25Q04,
  FM25Q08, FM25Q04A, FM25M04A, FM25Q08A, FM25M08A, FM25M16A, FM25M32B, FM25M64A,
  P25D05H, P25D10H, P25D20H, P25D40H, P25D80H, PY25Q64HA, PY25Q32HB, A25L10PU,
  A25L20PU, A25L040, A25LQ080, A25L080, A25LQ16, A25LQ32, A25L032, A25LQ64, ES25P10,
  ES25P20, ES25P40, ES25P80, ES25P16, ES25P32, ES25M40A, ES25M80A, ES25M16A,
  DQ25Q64AS, DQ25Q128AL, MD25D20, MD25D40, LE25U20AMB, LE25U40CMC, ZD25Q16A, ZD25Q32A,
  ZD25Q64A, ZD25Q128A, ZD25Q16B, ZD25Q32B, ZD25Q64B, ZD25Q128B, PCT25VF010A,
  PCT25VF020B, PCT25VF040B, PCT25VF080B, PCT25VF016B, PCT25VF032B, PCT25VF064C,
  PCT26VF016, PCT26VF032, ZD25Q16C, ZD25Q32C, ZD25Q64C, ZD25Q128C.
- `tests/norsim.c` checks that every SPI NOR chip of the table is detected by its own
  JEDEC ID.

## 1.10.1 — 2026-11-03

- SPI NAND erase: a block that fails to erase fails the whole erase. The next block
  overwrote the status, so `-e` ended with `OK` unless the failed block was the last
  one (#18). The erase still goes on with the other blocks, prints each failed block
  and the number of failed blocks at the end.

## 1.10.0 — 2026-11-03

- 54 more SPI NAND chips, 135 in all: GIGADEVICE GD5F2GQ4UA, GD5F1GQ4RE, GD5F2GQ5UE,
  GD5F2GQ5RE, GD5F1GM7UE, GD5F1GM7RE, GD5F2GM7UE, GD5F2GM7RE; ESMT F50D1G41LB(2M);
  WINBOND W25N01KV, W25N01KW, W25N01GW, W25N04KV, W25N04KW; MXIC MX35LF2G14AC,
  MX35UF1G14AC, MX35UF2G14AC, MX35LF1G24AD, MX35LF2G24AD, MX35LF4G24AD,
  MX35LF2G24AD-Z4I8, MX35LF4G24AD-Z4I8, MX35UF1GE4AD, MX35UF2GE4AD, MX35UF4GE4AD;
  KIOXIA TC58CVG0S3HRAIJ, TH58CVG3S0HRAIJ; MICRON MT29F1G01AA, MT29F2G01AA,
  MT29F4G01AA, MT29F2G01ABB, MT29F4G01ABA, MT29F4G01ABB; HEYANG HYF1GQ4UTACAE,
  HYF2GQ4UTACAE; FM FM25S02A; XTX XT26G02C; FORESEE F35UQA512M, F35UQA001G,
  F35UQA002G, F35SQA512M, F35SQA001G, F35SQA002G, FS35ND01GS1Y2, FS35ND02G-S3Y2,
  FS35ND04G-S2Y2; DS DS35Q1GB, DS35M1GA, DS35M2GA, DS35Q2GB, DS35Q2GBS, DS35M2GB,
  DS35M2GBS; XINCUN XCSP1AAWH-NT.
- `tests/nandsim.c` checks that every SPI NAND chip of the table is detected by its own
  ID.
## 1.9.2 — 2026-11-03

- CH341A: a long SPI stream goes to the programmer in parts of at most 16 KB, and
  the USB timeout grows with the transfer size. A whole 64 KB NOR sector in one
  transfer needed more than the flat 1 s timeout, so reading chips with 64 KB
  sectors failed on the first sector. A failed NOR read prints where it stopped
  instead of a wrong byte count.
- SPI NAND: a page whose data is all `0xFF` is not programmed: it would change
  nothing on the chip, and an erased page stays erased (its ECC bytes too).
  Writing images with empty areas is faster. With `-k` such a page is still read
  first, so a BAD page under it is skipped as on a read.
- SPI NAND: waiting for the chip to finish a page read, program or block erase
  stops after 3 s with an error; a chip that stays busy (or is not connected) hung
  the program before. A write that stops reports the bytes actually written (the
  count was always the full length), and a stopped erase does not print 100%.
- Tests: `tests/ch341sim.c` runs `ch341a_spi.c` against a fake libusb that plays a
  CH341A at its real stream speed; nandsim checks the `0xFF` pages and a chip that
  stays busy.

## 1.9.1 — 2026-11-03

- The banner shows the program name and version only.

## 1.9.0 — 2026-11-03

From PR #1 by @atom-bomb, ported from upstream SNANDer (#5):

- SPI NAND chips are matched by a 3-byte ID when the table gives one. New chips:
  Winbond W25N02KV, Macronix MX35LF2GE4AD and XTX XT26G01C; Winbond W25N01G and
  W25M02G are now told apart from W25N02KV by the third ID byte.
- `-k` skips BAD SPI NAND pages: a page that fails ECC on read or fails to program
  is passed over and the data goes on in the next page, so the data after it shifts
  by a page. Not together with `-I`. A page that failed to program is remembered for
  the `-v` of the same run; a later `-k` read does not know it and may read it as
  data, so its dump is shifted from that page on.

Fixed in the port (the same code in upstream SNANDer):

- `-k` never goes past the chip end: a skipped page near the end made the write wrap
  to page 0 and overwrite the start of the chip; now such a write or read fails. Each
  skipped page is printed.
- A `-k` write stays inside `-a`/`-l` (without `-l`, up to the chip end): the shifted
  data went on into the next partition, which was not erased. Give `-l` the partition
  size; a write that needs more room fails.
- Without `-k`, an ECC error in the read that a page write starts with does not fail
  the write, as before.
- After a skipped page the rest of the data starts at the next page boundary: a last
  chunk shorter than a page was written into the same failed page again.
- The fallback READ ID without the dummy byte (Toshiba/Kioxia) checks the third ID
  byte too: an unknown `EF AA xx` chip was detected as W25N01G.
- A page that failed ECC is not kept in the driver page cache, so reading it again
  fails again instead of returning the cached data.
- `tests/nandsim.c`: an SPI NAND chip simulated at the SPI command level runs
  `spi_nand_flash.c` in `tests/run.sh` (detection, write and read, `-k`).

## 1.8.2 — 2026-11-02

- `EEPROM_SUPPORT=y` build: the Microwire EEPROM info line printed a pointer to an
  array of a function that had already returned (`-Wdangling-pointer`); the number is
  formatted in `mw_init()` itself now. CI fails on warnings in this build too (#10).

## 1.8.1 — 2026-11-02

- Verify after write (`-v`) compares the data read back with the file byte by byte
  (the file in 64 KiB chunks, so the memory use stays one image) and prints the address
  of the first difference; before, a mismatch could be reported as `OK` (#2). A read
  shorter than requested fails, in `-r` too.
- `-u` on a chip without unprotect support (SPI NAND, EEPROM) prints a message and
  fails instead of crashing; `-u` with another operation is a conflicting-options
  error; `-u` is available in the `EEPROM_SUPPORT=y` build too (#3).
- The exit code is 0 only on success and 1 on any failure: chip not found, operation
  error, verify mismatch, file or memory error, unknown option, no operation (#4).
- SPI NAND write: the driver counted the written bytes into a pointer instead of the
  counter, so every write ended with `Status: BAD(0)` and `-v` never ran; it also went
  on after a failed page. Now the count is right and a failed page stops the write. A
  write is `OK` only when all the bytes were written (a partial NOR write was `OK`).
- `-w` writes only the bytes of the file: with `-l` longer than the file the rest of
  the buffer was written too; an empty file is an error.
- The banner names the program MicroSNANDer, based on SNANDer by McMCC.
- `tests/run.sh`: tests of `main.c` against a test double of the programmer, run in CI.

## 1.8.0 — 2026-11-02

- Static release binaries for Linux, linked against musl with libusb 1.0.30 built in
  (no udev), so they run whatever the libc of the system: `microsnander-linux-x86_64`
  and `microsnander-linux-arm` (32-bit ARM, ARMv5TE and later, soft-float) for cameras
  and boards (#6).
- `src/Makefile` takes `CC`, `CFLAGS`, `LDFLAGS`, `LIBUSB_CFLAGS` and `LIBUSB_LIBS`
  from the command line, for cross and static builds (#6).
- CI builds every push and pull request; the one release `latest` replaces the 2022
  development build (#6).

## 1.7.4

- Based on SNANDer 1.7.4 by McMCC: SPI NOR/NAND programmer for CH341A, stripped down.
- `-u`: remove the write protection of SPI NOR chips.
- New SPI NOR chips: FM25Q16/32/64/128, GM25Q64A, GM25Q128A, XM25QH64C, XM25QH128C.
