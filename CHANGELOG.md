# Changelog

The version lives in `_VER` in `src/main.c`. Binaries are published only as the
[`latest`](https://github.com/OpenIPC/microsnander/releases/latest) release, rebuilt
from every push to `main`; this file is the change history.

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
