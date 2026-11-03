# Changelog

The version lives in `_VER` in `src/main.c`. Binaries are published only as the
[`latest`](https://github.com/OpenIPC/microsnander/releases/latest) release, rebuilt
from every push to `main`; this file is the change history.

## 1.20.1 — 2026-11-04

- Faster SPI NAND reads: the ECC check of a page takes the status its ready poll has already
  read, with no second status read per page. CH347: CS low goes in the same USB transfer as
  the command after it, one round trip less for every SPI transaction. 1 MB from a W25N01G
  through the CH347: 1.93 s before, 1.48 s now. The CH341A is bound by its own SPI clock
  (about 110 KB/s, as with SPI NOR).

## 1.20.0 — 2026-11-04

- Self-test in the web mode and the Android app: a **Self-test** button runs `-t` on the
  first block of the chip — Detect first, then a question naming the chip found, and only
  that chip is tested. The protection and the verdict (`OK`, `PROTECTED`, `FAILED`) are the
  message; a block that does not come back is saved in the dump directory (`-o`, the app's
  dumps). `ms_selftest()` in the operations; the self-test keeps its lines for them.

## 1.19.1 — 2026-11-04

- SPI NAND: the FORESEE F35SQA001G and F35SQA002G are fully unlocked. They power up with the
  block lock register at 0x7c (BP3..BP0 and TB at bits 6..2); the unlock kept BP3 and TB
  (0x44), and the first 8 MB took no erase and no program. After the unlock of every maker,
  BP bits left in the register are cleared as well, and a register that stays locked is
  told instead of erases and programs that fail.

## 1.19.0 — 2026-11-04

- `-t`: a short self-test of the chip, with nothing to prepare. It reads the protection
  (SPI NOR: BP, TB, CMP, SRP; SPI NAND: the block lock register when found), then takes one
  block — the first by default, where a locked loader usually is, or the one at `-a` —
  keeps its data, erases it, writes a pattern, reads it back and puts the data back. One
  line at the end: `OK` (exit 0), `PROTECTED` (2) or `FAILED` with the first wrong address
  (1). With protection bits set it stops before any erase and leaves them as they are
  (`-u` clears them); a block that takes the commands and keeps its data with no bit set is
  `PROTECTED` too. Old data that does not come back is saved to
  `selftest-<chip>-0x<address>.bin`. SPI NAND: block 0 only, its spare area besides the ECC
  is not kept; an SST26 is told that its blocks lock at power-up. Not with `-d`, `-I`, `-k`.
  With the EZP programmers (whole-chip erase only) it reads the block twice.

## 1.18.2 — 2026-11-04

- Unprotect (`-u`, Clear protection in the web mode and the app) also clears SRP0 and TB
  when no BP bit is set: a chip with SRP0 alone (SR 80 00 00) kept it and the message said
  the protection was cleared. Every status register is read back after its write; one that
  stays says why: SRP0 with the WP# pin low, SRP1 (locked until a power cycle), or a chip
  that does not take the write. Bit 6 of SR1 is kept (QE on Macronix-type chips).

## 1.18.1 — 2026-11-03

- Erase and Write ask nothing and open no file picker without a chip (#42). The Android
  app runs Detect first and names the chip found in its question; the web page keeps
  Erase and Write off until a chip is known and runs Detect again before the question.
- `-h` text without missing spaces. The READMEs say what `-W` refuses (the operations,
  `-v`, `-S`, `-d`, `-I`, `-k`) and that `-K` and `-o` go with `-W` only.

## 1.18.0 — 2026-11-03

- No libusb: microsnander talks to the programmers through the Linux usbfs itself
  (`src/usbfs.c`: devices from `/dev/bus/usb`, bulk and control transfers by ioctl, URBs
  for the asynchronous CH341A transfers, the kernel driver let go and given back with
  `USBDEVFS_DISCONNECT`/`CONNECT`, the file descriptor of the Android app). The drivers
  do not change. The static binaries need no libusb build and are smaller (x86_64 220 KB
  instead of 319 KB); the Android app builds without it too. `make LIBUSB=1` builds
  against libusb-1.0 as before, CI builds both.

## 1.17.1 — 2026-11-03

- Write in the web mode and the Android app goes from the file in parts of 64 KB, read
  again for the verify, instead of the whole image in memory: a SPI NAND image of
  hundreds of MB on a TV box with little memory. The web mode keeps the uploaded file,
  without a name, in the `-o` directory until the write ends; no place for it there is
  an error before the write starts.

## 1.17.0 — 2026-11-03

- A warning about 1.8 V chips: the supply voltage is taken from the chip name (56 SPI NOR
  and 20 SPI NAND chips of 1.8 V, 15 SPI NOR chips of 1.65-3.6 V, by their datasheets).
  After a 1.8 V chip is found the console prints a warning, the web page lights the Chip
  lamp red and shows it, the Android app shows it too; `/api/state` and the app get the
  `voltage` and the `warning`. With the CH341A the warning also reminds that the black
  board puts 5 V on the chip lines unless it is modified for 3.3 V.
- Chip names: the Micron N25Q entries are N25Q..A11 (1.8 V) and N25Q..A13 (3 V), as the
  parts are named, instead of one name for both; `W25Q80` (EF 50 14) is `W25Q80BW`.
- The EEPROM support is removed: the I2C (24Cxx) and Microwire (93Cxx) EEPROM code of
  the original SNANDer (`make EEPROM_SUPPORT=y`, options `-E`, `-8`, `-f`) and the
  build option (#14). The release binaries were built without it and do not change.
  The READMEs say that EEPROM chips are not supported.

## 1.16.4 — 2026-11-03

- Release: the signer check also reads the `V2 Signer: certificate SHA-256 digest` line
  of `apksigner` from build-tools 37, the one on the CI runner. Its output in the 1.16.3
  run showed the project's certificate, under that name; 1.16.3 was not published.

## 1.16.3 — 2026-11-03

- Release: the signer check takes the digest from any `Signer ... certificate SHA-256
  digest` line of `apksigner` (newer versions name the signer by its SDK range) and
  prints the whole output when there is none. 1.16.2 was not published either: its
  check found no signer line.

## 1.16.2 — 2026-11-03

- Release: the check of the APK signer tries the build-tools from the newest down until
  `apksigner` runs, and prints its output when none does. On the CI runner the newest
  `apksigner` gave no signer, so 1.16.1 was not published; this is the first release with
  `microsnander-android.apk`.

## 1.16.1 — 2026-11-03

- The Android app is in the release: `microsnander-android.apk`, signed with the
  project's own release key (a CI secret), next to the Linux binaries; the release checks
  its signature and version. The debug APK stays a CI artifact.
- The release key is a secret of the `release` environment, which only `main` may use:
  branch and pull request builds never reach it. The release checks that the APK is
  signed with the project's certificate; a fork without the key still publishes the
  Linux binaries.
- Android TV banner: the name on two lines, "Micro" / "SNANDer", in a larger size
  (OpenIPC brand 0.5.0).
- Checked on hardware: write of a 1 MB file from the file picker with verify, on an
  Android 10 TV box with a CH341A and a 3.3 V W25Q128 (erase 55 s, write and verify 52 s);
  two full reads identical.

## 1.16.0 — 2026-11-03

- Android app (`android/`, minSdk 21): Detect, Read, Erase, Write and Registers as in
  the web mode, with a CH341A, CH347 or EZP programmer on the USB host port of a phone or
  TV box, no root. The dump `flash-YYYYMMDDHHMM-CHIP.bin` goes to Downloads; Save as… and
  Share. The page style of the web mode; launcher icon and TV banner of the OpenIPC apps.
- The same C code in both: the operations moved from `web.c` to `src/ops.c`, and the
  drivers open their USB device through `src/usb.c`, which in the app takes the file
  descriptor of the Android USB manager (`libusb_wrap_sys_device`, no device discovery).
- CI builds the app (lint without warnings) and keeps a debug APK as an artifact.
- `tests/ch347sim.c`: a CH347 opened from a file descriptor.

## 1.15.0 — 2026-11-03

- Web mode: `-W [address:]port` runs microsnander as a service with a page in the
  browser: Detect, Read, Erase, Write and Registers for the programmer that is
  connected. `-K <password>` asks for a password (HTTP basic authentication), `-o <dir>`
  is where Read saves the dumps.
  - Read: the whole chip, or the `-a`/`-l` range given at the start, into
    `flash-YYYYMMDDHHMM-CHIP.bin` (the range adds `-0xADDRESS-0xLENGTH`), which the
    browser downloads too.
  - Erase: the whole chip. Write: a file from the browser, from address 0, then read
    back and compared.
  - Registers: the bits of status registers 1–3 and Clear protection (as `-u`), for SPI
    NOR through the CH341A or the CH347.
  - One operation at a time, in a thread of its own; the page shows its progress on a
    gauge. The programmer is opened for each operation and closed after it.
  - The page has the style of the other OpenIPC tools: lamps, gauge, display, brass
    buttons, and a lamp for every status register bit.
  - A second read in the same minute does not replace the first dump: `-2`, `-3`, ...
  - Access: an operation needs a header that only the page sends (no erase or write from a
    page of another site); a request has to name this machine by its IP address,
    `localhost` or its host name (DNS rebinding); only regular dump files are served, never
    links; an empty `-K` is refused. A thread for each connection, up to 16, so a slow
    client does not hold the others; errors of `accept()` do not stop the service.
  - `-d`, `-I`, `-k` and the EEPROM options are refused with `-W`.
- `-a` and `-l` that are not numbers are refused (they were taken as 0).
- `src/web.c`: a small HTTP/1.0 server, no library needed. The page is
  `src/web_page.html`, built into the program.
- `tests/websim.c` runs `web.c` with a fake flash: the requests, the password, the
  operations, the dump names and downloads.
- Checked on hardware: CH347 (detect, registers, read of 16 MB in 7 s, download, erase,
  write and verify), CH341A (registers, clear protection) and EZP2019+ (detect; the
  registers are reported as not available).

## 1.14.1 — 2026-11-03

- SPI NOR writes are 2–3 times faster on the CH341A and the CH347 (#29). Writing
  was limited by USB round trips, not by the SPI clock:
  - the protection is cleared once before a write, not before every 256-byte page
    (three status register reads a page);
  - short writes while CS is low (an opcode, an address) are held in
    `spi_controller.c` and go out with the data, the read or the CS high that follows,
    in one transfer: a page program or a status read is one transfer instead of up to
    five;
  - CH347: a short command and its answer (a status read) are one full-duplex
    transfer;
  - CH341A: chip select goes out with the next stream instead of as a USB transfer of
    its own.
- Measured on hardware with the same chip (W25Q128, 16 MB) and a full-chip write:
  CH347 455 s → 152 s, CH341A 584 s → 256 s. Read-backs identical.
- `tests/spisim.c` checks the held writes against a recording programmer;
  `tests/norsim.c` checks that a write clears the protection once; `tests/ch347sim.c`
  checks the full-duplex transfer.

## 1.14.0 — 2026-11-03

- EZP2019, EZP2019+ and EZP2023 programmers (USB `1fc8:310b`, `310c` or `310d`; the
  ID does not tell the model, an EZP2019+ reports `310b`), `-P ezp`,
  for SPI NOR chips up to 16 MB. They run the SPI bus in their own firmware and take
  whole operations only. The chip is found in the SPI NOR table by the JEDEC ID that
  the programmer reports. Then:
  - `-r`, `-w` and `-v` work as with the other programmers;
  - `-e` erases only the whole chip; erasing a part is refused;
  - `-w` reads the range first and refuses to write over data that is not erased;
  - the status registers can be neither read nor written, so there is no `-u`;
  - no SPI NAND.
- A read stream of these programmers starts right only at a multiple of 256: the read
  starts there and drops the bytes before the address.
- Checked on hardware: EZP2019+ (`1fc8:310b`) with a W25Q128 (16 MB). Chip erase 40 s, full read
  135 s, full write 137 s (plus about 136 s each for the erased check and `-v`). Reads
  at odd addresses and up to the chip end, and a 4097-byte write at `0x1001`, are
  identical to the expected data.
- `tests/ezpsim.c` runs `ezp.c` against a fake libusb that plays the programmer.
- README: the EZP programmers in the requirements, `-P ezp` and the udev rules.

## 1.13.1 — 2026-11-03

- SPI NOR unprotect keeps the quad enable bit (QE) of status register 2 and clears
  only its protection bits, SRP1 and CMP (#30). QE protects nothing. Clearing it could
  stop a SoC that boots with quad reads, and on parts with QE fixed at 1 (W25Q128JV
  "Q") the unprotect wrote status register 2 again before every page program. Found on
  hardware: a W25Q128JV with QE fixed on a CH341A.
- `tests/norsim.c`: a chip with QE fixed at 1 (status register 2 is never written)
  and a chip protected with CMP (CMP cleared, QE kept).

## 1.13.0 — 2026-11-03

- CH347 programmers: CH347T (`1a86:55db`, mode 1, SPI interface 2) and CH347F
  (`1a86:55de`, interface 4), in the same build as the CH341A. The kernel driver, if
  one has taken the interface, is detached for the run and attached back after it.
- `-P ch341a|ch347|auto` chooses the programmer. By default (`auto`) the one
  connected is used; with several connected microsnander asks for `-P`.
- `-S 60M|30M|15M|7.5M|3.75M|1.875M` sets the SPI clock of the CH347; the default is
  30 MHz, within the 50 MHz of the plain READ command of most chips.
- `-S` with the CH341A, whose clock is fixed, is an error, given before the
  programmer is opened. Two programmers of the same kind connected: refused, as it is
  not known which one would be opened.
- Every CH347 answer is checked: a write must be acknowledged with status 0, a read
  answer must carry the read command and as many bytes as its length says.
- EEPROM chips (`EEPROM_SUPPORT=y` builds) stay CH341A-only.
- Checked on hardware: CH347T with a W25Q128JV (16 MB): chip erase 34 s, full read
  7.2 s at 30 MHz and 5.2 s at 60 MHz (the CH341A reads 8 MB in 68 s), full write
  485 s (writes are limited by USB round trips per page, #29), write with verify, all
  read-backs identical.
- `tests/ch347sim.c` runs `ch347_spi.c` against a fake libusb that plays a CH347:
  configuration, chip select, writes and reads in 510-byte packets, CH347F interface.
- README: the CH347 in the requirements, `-P` and `-S` in the usage, one udev rule
  for both programmers.

## 1.12.2 — 2026-11-03

- SPI NOR: the chip is ready when the write-in-progress bit of the status register is
  clear, whatever the other bits are. Before, a status with only the write enable latch
  set counted as busy, and a busy chip with a protection or quad enable bit set counted
  as ready, so a command could go to a chip still erasing or programming (#27).
- SPI NOR: the busy timeouts are wall-clock time. They were a count of status reads,
  and a status read over USB takes much longer than the delay between them, so a sector
  erase on a chip that never got ready waited many minutes. A timeout now fails the
  erase, and the write reports only the pages that finished.
- SPI NOR unprotect (`-u`, and before an erase or write): a status register 2 or 3 that
  reads `0xFF` is taken as missing and left alone. On chips without them (XM25QH64A) the
  unprotect wrote to a register that does not exist, the chip ignored it, and the write
  enable latch stayed set, so `-u` printed a timeout and the next erase hung. Found on
  hardware: CH341A, XM25QH64A. The latch is also cleared after every unprotect write.
- `tests/norsim.c`: a chip without status registers 2 and 3, a chip with the quad
  enable bit set and busy after every erase and program, a chip that never gets ready;
  every simulated chip checks that only status reads go to it while it is busy.

## 1.12.1 — 2026-11-03

- CH341A: the static release binaries detach the kernel driver again. The detach was
  under `__gnu_linux__`, which the musl toolchain does not define, so on kernels where
  `spi-ch341` binds the programmer the release binaries stopped with
  `LIBUSB_ERROR_BUSY` (#24). Found on hardware: CH341A, kernel 7.0. The kernel driver is
  attached again when microsnander exits.

## 1.12.0 — 2026-11-03

- SST25VF-type SPI NOR chips without a 256-byte page program are written with a byte
  program for an odd first or last byte and AAI word program for the rest; their
  status register protection (BP3, BPL) is cleared like on other chips (#22). This is
  slow on the CH341A: one SPI command per two bytes.
- SST26-type SPI NOR chips: the protection is cleared with the global unlock ULBPR, and
  they are erased in 4 KB sectors, as their blocks at the start are 8 KB and 32 KB.
- Back in the table: PCT25VF020B, PCT25VF040B, PCT25VF080B, PCT25VF016B, PCT25VF032B,
  PCT26VF016, PCT26VF032 (291 SPI NOR chips). PCT25VF010A stays out: it has no JEDEC
  READ ID.
- SPI NOR: the write enable goes after the unprotect step before a page program and a
  chip erase. Clearing the protection writes the status register, which clears the
  write enable latch, so the first page program or a chip erase of a protected chip did
  nothing.
- `tests/norsim.c` simulates an ordinary, an SST25VF-type and an SST26-type chip and
  checks chip erase, sector erase, and a write at an odd address with an odd length.

## 1.11.1 — 2026-11-03

- SPI NAND OOB size: 128 bytes for TOSHIBA TC58CVG0S3H and TC58CVG1S3H (was 64), 256
  bytes for TOSHIBA TC58CVG2S0H and KIOXIA TC58CVG2S0HRAIJ (was 128). It changes raw
  dumps with `-d`, which now take the whole spare area of each page. A raw dump of these
  chips made by an older version has the old page stride and cannot be written back as
  it is: pad each page with `0xFF` to the new OOB size first.
- Raw writes (`-d -w`) take whole pages of data + OOB only, starting on a page; a raw
  image made with another OOB size is refused instead of landing shifted on every page.
- README: a section on chips whose data other sources give differently and that are
  not checked on hardware yet; the title is MicroSNANDer.

## 1.11.0 — 2026-11-03

- 157 more SPI NOR chips, 284 in all: GD25Q20C, GD25Q40C, GD25Q80C, GD25LQ80C,
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
  ZD25Q64A, ZD25Q128A, ZD25Q16B, ZD25Q32B, ZD25Q64B, ZD25Q128B, PCT25VF064C, ZD25Q16C,
  ZD25Q32C, ZD25Q64C, ZD25Q128C.
- Not added: SST25VF-type chips without 256-byte page program (PCT25VF010A to
  PCT25VF032B: they need byte or AAI programming) and SST26-type chips whose blocks
  stay locked without the ULBPR command (PCT26VF016, PCT26VF032).
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
