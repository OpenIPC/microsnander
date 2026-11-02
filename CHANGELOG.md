# Changelog

The version lives in `_VER` in `src/main.c`. Binaries are published only as the
[`latest`](https://github.com/OpenIPC/microsnander/releases/latest) release, rebuilt
from every push to `main`; this file is the change history.

## 1.8.1 — 2026-11-02

- Verify after write (`-v`) compares the data read back with the data written, byte by
  byte, and prints the address of the first difference; before, a mismatch could be
  reported as `OK` (#2).
- `-u` on a chip without unprotect support (SPI NAND, EEPROM) prints a message and
  fails instead of crashing; `-u` with another operation is a conflicting-options
  error; `-u` is available in the `EEPROM_SUPPORT=y` build too (#3).
- The exit code is 0 only on success and 1 on any failure: chip not found, operation
  error, verify mismatch, file or memory error, unknown option, no operation (#4).
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
