#!/bin/sh
#
# tests/run.sh — main.c against tests/stub.c (no programmer needed): exit
# codes, option conflicts, -u on chips without unprotect, verify after write.
# Then spi_nand_flash.c against the SPI NAND simulator tests/nandsim.c:
# detection by ID, write and read, skipping BAD pages with -k, pages of
# 0xFF, a chip that stays busy. Then ch341a_spi.c against a fake libusb
# (tests/ch341sim.c): long streams in parts inside the USB timeout. Then the
# SPI NOR probe (tests/norsim.c): every chip of the table by its JEDEC ID;
# ch347_spi.c against a fake libusb (tests/ch347sim.c): the CH347 packets;
# ezp.c against a fake libusb (tests/ezpsim.c): the EZP commands and streams;
# spi_controller.c against a recording programmer (tests/spisim.c): short
# writes held while CS is low go out in one transfer; web.c with a fake
# flash (tests/websim.c): the requests, the operations, the dump names.
#
#   tests/run.sh            (CC=<compiler> to choose the compiler)
#
set -u
T=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
${CC:-cc} -std=gnu99 -Wall -Werror -I"$T/../src" -o "$W/ms" "$T/../src/main.c" "$T/../src/sigmastar.c" "$T/../src/selftest.c" "$T/../src/timer.c" "$T/stub.c" || exit 1
${CC:-cc} -std=gnu99 -Wall -Werror -O1 -DSPI_NAND_OIP_TIMEOUT_MS=50 -o "$W/nandsim" "$T/nandsim.c" "$T/../src/timer.c" || exit 1
${CC:-cc} -std=gnu99 -Wall -Werror -O1 -I"$T/../src" ${LIBUSB_CFLAGS:-} -o "$W/ch341sim" "$T/ch341sim.c" || exit 1
${CC:-cc} -std=gnu99 -Wall -Werror -O1 -o "$W/norsim" "$T/norsim.c" "$T/../src/timer.c" || exit 1
${CC:-cc} -std=gnu99 -Wall -Werror -O1 -I"$T/../src" ${LIBUSB_CFLAGS:-} -o "$W/ch347sim" "$T/ch347sim.c" || exit 1
${CC:-cc} -std=gnu99 -Wall -Werror -O1 -I"$T/../src" ${LIBUSB_CFLAGS:-} -o "$W/ezpsim" "$T/ezpsim.c" "$T/../src/timer.c" || exit 1
${CC:-cc} -std=gnu99 -Wall -Werror -O1 -I"$T/../src" -o "$W/spisim" "$T/spisim.c" || exit 1
${CC:-cc} -std=gnu99 -Wall -Werror -O1 -I"$T/../src" -o "$W/usbfssim" "$T/usbfssim.c" || exit 1
${CC:-cc} -std=gnu99 -Wall -Werror -O1 -I"$T/../src" -o "$W/ispsim" "$T/ispsim.c" || exit 1
make -s -C "$T/../src" web_page.h || exit 1
${CC:-cc} -std=gnu99 -Wall -Werror -O1 -I"$T/../src" -o "$W/websim" "$T/websim.c" -pthread || exit 1
cd "$W"; B=./ms; fail=0
# t <expected exit code> <description> [VAR=value ...] <command>
t() { exp=$1; d=$2; shift 2; out=$(env "$@" 2>&1); got=$?
  if [ "$got" = "$exp" ]; then echo "ok   $d (exit $got)"; else echo "FAIL $d: exit $got, want $exp"; echo "$out" | tail -3; fail=1; fi; }
rm -f chip.img; head -c 65536 /dev/urandom > img.bin; head -c 1000 /dev/urandom > small.bin
t 0 "-h"                              $B -h
t 1 "no operation"                    $B
t 1 "unknown option"                  $B -Z
t 1 "conflict -u -i"                  $B -u -i
t 1 "conflict -i -u"                  $B -i -u
t 1 "conflict -u -r"                  $B -u -r x.bin
t 1 "conflict -k -I"                  $B -k -I -r x.bin
t 1 "-P unknown programmer"           $B -P ft2232 -i
t 0 "-P ch341a"                       $B -P ch341a -i
t 0 "-P ezp"                          $B -P ezp -i
t 0 "-W 8080 (web mode)"              $B -W 8080
t 1 "-W with -r is an error"          $B -W 8080 -r x.bin
t 1 "-W with -v is an error"          $B -W 8080 -v
t 1 "-K without -W is an error"       $B -K pw -i
t 1 "-W with -d is an error"          $B -W 8080 -d
t 1 "-W with -k is an error"          $B -W 8080 -k
t 1 "-W with -I is an error"          $B -W 8080 -I
t 1 "-l that is not a number"         $B -r x.bin -l 12abc
t 1 "-a that is not a number"         $B -r x.bin -a zz
t 1 "-o without -W is an error"       $B -o /tmp -i
$B -W 127.0.0.1:8080 -K pw -o /srv -a 0x1000 -l 0x2000 | grep -q '^web: 127.0.0.1:8080 key=pw dir=/srv addr=0x1000 len=0x2000' && echo "ok   -W passes the address, password, dump directory and range" || { echo "FAIL -W arguments"; fail=1; }
$B -h | grep -q '^ -W ' && $B -h | grep -q '^ -K ' && $B -h | grep -q '^ -o ' && echo "ok   -h lists -W, -K and -o" || { echo "FAIL -h without -W/-K/-o"; fail=1; }
t 1 "-P ezp -S 15M is an error"       $B -P ezp -S 15M -i
t 1 "-S unknown speed"                $B -S 100M -i
t 0 "-P ch347 -S 15M"                 $B -P ch347 -S 15M -i
t 1 "-S with the CH341A is an error"  $B -P ch341a -S 15M -i
t 1 "-S with auto (CH341A) is an error" $B -S 15M -i
t 1 "-m unknown mode"                 $B -m mstar -i
t 0 "-m sigmastar -i (CH341A, auto)"  $B -m sigmastar -i
t 0 "-P ch347 -m sigmastar -i"        $B -P ch347 -m sigmastar -i
t 1 "-P ezp -m sigmastar: no I2C"     $B -P ezp -m sigmastar -i
t 1 "-m sigmastar, nothing at 0x49"   MS_I2C=none $B -m sigmastar -i
t 0 "-m sigmastar -S 400K (CH341A)"   $B -m sigmastar -S 400K -i
t 0 "-S before -m is the I2C clock"   $B -S 750K -m sigmastar -i
t 1 "-m sigmastar -S 15M is an error" $B -m sigmastar -S 15M -i
t 1 "-W with -m is an error"          $B -W 8080 -m sigmastar
t 1 "-P ezp -q: no I2C"               $B -P ezp -q
t 1 "-q -i without -m sigmastar"      $B -q -i
t 0 "-m sigmastar -i -q"               $B -m sigmastar -i -q
$B -m sigmastar -i -q | grep -q '^40: .* 49 ' && $B -m sigmastar -i -q | grep -q 'SigmaStar ISP through' && echo "ok   -q before -i: the bus, then the operation" || { echo "FAIL -q with -i"; fail=1; }
$B -q | grep -q '^40: .* 49 ' && $B -q | grep -q '^50: .* 59 ' && $B -q | grep -q '^00: -- ' && echo "ok   -q lists the I2C addresses that answer" || { echo "FAIL -q"; fail=1; }
$B -h | grep -q '^ -m sigmastar' && $B -h | grep -q '^ -q ' && echo "ok   -h lists -m and -q" || { echo "FAIL -h without -m/-q"; fail=1; }
MS_PROGRAMMER=1 $B -P ch347 -i | grep -q '^programmer: ch347' && MS_PROGRAMMER=1 $B -i | grep -q '^programmer: ch341a' && echo "ok   -P chooses the programmer, auto by default" || { echo "FAIL programmer choice"; fail=1; }
$B -h | grep -q '^ -P ' && $B -h | grep -q '^ -S ' && echo "ok   -h lists -P and -S" || { echo "FAIL -h without -P/-S"; fail=1; }
$B -h | grep -q '^ -k ' && echo "ok   -h lists -k" || { echo "FAIL -h without -k"; fail=1; }
t 0 "-i detected"                     $B -i
# -t: the self-test of the first block (or -a), its data put back
head -c 65536 /dev/urandom > chip.img; cp chip.img before.img
t 0 "-t on a good chip"               $B -t
$B -t | grep -q '^Self-test: OK' && cmp -s chip.img before.img && $B -t | grep -q 'block 0x0 + 0x1000' && echo "ok     the first block, OK, and the chip is as it was" || { echo "FAIL -t OK"; fail=1; }
$B -t -a 0x8123 | grep -q 'block 0x8000 + 0x1000' && echo "ok     -a 0x8123: the block at 0x8000" || { echo "FAIL -t -a"; fail=1; }
t 2 "-t, protected by BP bits"        MS_SR=1c0000 MS_PROTECT=1 $B -t
t 2 "-t Macronix, BP3 alone protects" MS_MACRONIX=1 MS_SR=20ff07 MS_PROTECT=1 $B -t
MS_MACRONIX=1 MS_SR=20ff07 $B -t | grep -q 'SR1 = 0x20: BP = 8' && echo "ok     -t Macronix names BP3" || { echo "FAIL -t Macronix BP3"; fail=1; }
t 0 "-t Macronix: QE is not a protection, ff and 07 are not read as registers" MS_MACRONIX=1 MS_SR=40ff07 $B -t
t 1 "-t Macronix: an erase that fails is a failure, not protection" MS_MACRONIX=1 MS_SR=40ff07 MS_FAIL=erase $B -t
cp before.img chip.img; MS_SR=1c0000 $B -t | grep -q 'PROTECTED, the registers protect part.*-u' && MS_SR=1c0000 $B -t | grep -q 'BP = 7, TB = 0' && cmp -s chip.img before.img && echo "ok     says PROTECTED, the bits and -u, and erases nothing" || { echo "FAIL -t protected"; fail=1; }
t 2 "-t, nothing changes, no bits set" MS_PROTECT=1 $B -t
MS_PROTECT=1 $B -t | grep -q 'no register bit says so' && echo "ok     a block that keeps its data with no bit set: told apart" || { echo "FAIL -t hidden protection"; fail=1; }
t 1 "-t, a write that does not take"  MS_CORRUPT=5 $B -t
cp before.img chip.img; MS_CORRUPT=5 $B -t | grep -q 'the write does not take: .* at 0x5,' && head -c 4096 before.img | cmp -s - selftest-W25Q80-0x0.bin && echo "ok     the bad byte and its address; the old block saved to selftest-W25Q80-0x0.bin" || { echo "FAIL -t corrupt"; fail=1; }
rm -f selftest-*.bin
cp before.img chip.img; MS_FAIL=stop $B -t > stop.log; [ $? = 1 ] && grep -q 'stopped by Ctrl+C' stop.log && cmp -s chip.img before.img && [ ! -e selftest-W25Q80-0x0.bin ] && echo "ok   -t stopped by Ctrl+C after the erase: the block gets its data back, exit 1" || { echo "FAIL -t Ctrl+C"; cat stop.log | tail -3; fail=1; }
rm -f selftest-*.bin stop.log
t 1 "-t, an erase that fails"         MS_FAIL=erase $B -t
MS_FAIL=erase $B -t | grep -q 'the erase command fails' && echo "ok     a failing erase command is FAILED, not PROTECTED" || { echo "FAIL -t erase fail"; fail=1; }
t 1 "-t on SPI NAND, not block 0"     MS_CHIP=nand $B -t -a 0x20000
t 1 "-t with -k is a conflict"        $B -t -k
t 1 "-t with -d is a conflict"        $B -t -d
MS_SST26=1 $B -t | grep -q 'SST26-type chip: its blocks are locked at power-up' && echo "ok     an SST26: its power-up locks are told" || { echo "FAIL -t sst26"; fail=1; }
t 0 "-P ezp -t: reads only"           $B -P ezp -t
$B -P ezp -t | grep -q 'READ OK.*erases only the whole chip' && echo "ok     the EZP: two reads, no block erase" || { echo "FAIL -t ezp"; fail=1; }
MS_CHIP=nand MS_LOCK=7c $B -t | grep -q 'locked at power-up (A0 = 0x7c, BP = 7)' && echo "ok     SPI NAND: the lock at power-up is told" || { echo "FAIL -t nand lock"; fail=1; }
t 1 "-t with -r is a conflict"        $B -t -r x.bin
t 1 "-W with -t is an error"          $B -W 8080 -t
$B -h | grep -q '^ -t ' && echo "ok     -h lists -t" || { echo "FAIL -h without -t"; fail=1; }
rm -f chip.img before.img
t 1 "-i chip not found"               MS_CHIP=none $B -i
t 0 "-u NOR"                          $B -u
t 1 "-u NOR fails"                    MS_FAIL=unprotect $B -u
t 1 "-u NAND: not supported, no crash" MS_CHIP=nand $B -u
t 0 "-uu NOR"                         $B -uu
t 0 "-u -u NOR"                       $B -u -u
t 1 "-uu NOR fails"                   MS_FAIL=qe $B -uu
t 1 "-uu NAND: not supported, no crash" MS_CHIP=nand $B -uu
t 1 "-uuu is an error"                $B -uuu
t 1 "conflict -uu -i"                 $B -uu -i
$B -u | grep -q 'clear QE' && { echo "FAIL -u clears QE"; fail=1; } || echo "ok     -u leaves QE"
$B -uu | grep -q 'clear QE' && echo "ok     -uu clears QE" || { echo "FAIL -uu without QE"; fail=1; }
t 0 "-e full"                         $B -e
t 1 "-e fails"                        MS_FAIL=erase $B -e
t 1 "-e len not block multiple"       $B -e -a 0 -l 100
t 0 "-w -v ok"                        $B -w img.bin -v
t 1 "-w -v mismatch at 0x1234"        MS_CORRUPT=4660 $B -w img.bin -v
$B -w img.bin -v >/dev/null; MS_CORRUPT=4660 $B -w img.bin -v | grep -q 'first difference at 0x0000000000001234' && echo "ok   mismatch address reported" || { echo "FAIL mismatch address"; fail=1; }
t 1 "-w -v mismatch in last byte"     MS_CORRUPT=65535 $B -w img.bin -v
printf "\000\000\000\000\000\000\000\000" > zero8.bin
t 1 "-w -v false OK of #2 (byte 3 differs, next bytes equal)" MS_CORRUPT=3 $B -w zero8.bin -v -l 8
t 0 "-w small file -v"                $B -w small.bin -v
t 1 "-w -v read fails"                MS_FAIL=read $B -w img.bin -v
t 1 "-w -v short read-back"          MS_FAIL=shortread $B -w img.bin -v
t 1 "-w write error"                 MS_FAIL=write $B -w img.bin
t 1 "-w partial write"               MS_FAIL=partial $B -w img.bin
t 1 "-w missing file"                 $B -w nope.bin
t 1 "-w empty file"                   $B -w /dev/null
printf 'abc' > abc.bin
t 0 "-w file shorter than -l, -v"     $B -w abc.bin -a 0x100 -l 0x1000 -v
$B -w abc.bin -a 0x100 -l 0x1000 | grep -q 'writing 0x3 bytes' && echo "ok   only the file bytes are written" || { echo "FAIL short file length"; fail=1; }
$B -w img.bin >/dev/null
t 0 "-r full"                         $B -r out.bin
cmp -s out.bin img.bin && echo "ok   -r gives the written image" || { echo "FAIL -r content"; fail=1; }
t 1 "-r short read"                  MS_FAIL=shortread $B -r out3.bin
t 1 "-r read fails"                   MS_FAIL=read $B -r out2.bin
t 1 "-r unwritable file"              $B -r /nonexistent/dir/out.bin
[ ! -e out2.bin ] && echo "ok   a read that failed leaves no half dump" || { echo "FAIL half dump left"; fail=1; }
printf 'earlier' > keep.bin; chmod 640 keep.bin
t 1 "-r fails over an earlier dump"   MS_FAIL=read $B -r keep.bin
[ "$(cat keep.bin)" = earlier ] && [ "$(ls keep.bin.* 2>/dev/null)" = "" ] && echo "ok   the earlier dump stays, no temporary file" || { echo "FAIL earlier dump or a temporary file"; fail=1; }
t 0 "-r over an earlier dump"         $B -r keep.bin
cmp -s keep.bin img.bin && [ "$(stat -c %a keep.bin)" = 640 ] && echo "ok   the dump replaces it, with its mode" || { echo "FAIL dump over an earlier one"; fail=1; }
(umask 027; rm -f mode.bin; $B -r mode.bin >/dev/null); [ "$(stat -c %a mode.bin)" = 640 ] && echo "ok   a new dump gets the mode of the umask" || { echo "FAIL mode of a new dump"; fail=1; }
printf 'earlier' > hard.bin; ln hard.bin alias.bin
t 0 "-r over a file with two names"   $B -r hard.bin
cmp -s alias.bin img.bin && echo "ok   the other name has the dump too" || { echo "FAIL other name keeps the old data"; fail=1; }
printf 'earlier' > ro.bin; chmod 440 ro.bin
printf 'earlier' > wo.bin; chmod 200 wo.bin
t 0 "-r over a write-only dump"       $B -r wo.bin
if [ "$(id -u)" != 0 ]; then
	t 1 "-r over a read-only dump"    $B -r ro.bin
	[ "$(cat ro.bin)" = earlier ] && echo "ok   a read-only dump stays" || { echo "FAIL read-only dump replaced"; fail=1; }
fi
ln -s keep.bin link.bin; t 0 "-r into a link" $B -r link.bin
[ -L link.bin ] && echo "ok   a link stays a link" || { echo "FAIL link replaced"; fail=1; }
t 0 "-w of a file of size 0 with data" $B -w /proc/self/status -l 8
# a range goes in parts of whole blocks, 64 KiB here; in one piece where parts would not do
dd if=/dev/urandom of=big.bin bs=1024 count=1024 2>/dev/null
big() {
	want=$1; shift
	o=$(MS_BIG=1 MS_LARGEST=1 "$@"); e=$?
	[ $e = 0 ] && printf '%s\n' "$o" | grep -q "^largest $want\$" && echo "ok   longest transfer $want: ${*#*ms}" ||
		{ echo "FAIL longest transfer not $want or exit $e: $*"; fail=1; }
}
big 0x10000 $B -w big.bin -v
big 0x10000 $B -r bigout.bin
cmp -s big.bin bigout.bin && echo "ok   1 MiB in parts: same data" || { echo "FAIL 1 MiB in parts"; fail=1; }
[ "$(MS_BIG=1 $B -r bigout.bin | grep -c Elapsed)" = 1 ] && echo "ok   one time for all the parts" || { echo "FAIL time printed by parts"; fail=1; }
MS_BIG=1 $B -r bigout.bin | grep -q 'Read 100% \[1048576\] of \[1048576\]' && echo "ok   one progress for all the parts" || { echo "FAIL progress of the parts"; fail=1; }
big 0xf000 $B -r part.bin -a 0x1000 -l 0xf000
big 0x10000 $B -r part.bin -a 0x1000 -l 0x20000
dd if=big.bin of=want.bin bs=4096 skip=1 count=32 2>/dev/null
cmp -s part.bin want.bin && echo "ok   a range that starts inside a block" || { echo "FAIL range inside a block"; fail=1; }
MS_BIG=1 MS_CORRUPT=524293 $B -w big.bin -v | grep -q 'first difference at 0x0000000000080005' && echo "ok   the difference in a later part" || { echo "FAIL address of a difference in a later part"; fail=1; }
big 0x100000 $B -k -r bigout.bin
big 0x100000 $B -d -r bigout.bin
big 0x100000 $B -P ezp -r bigout.bin
cat big.bin | big 0x100000 $B -w /dev/stdin
./nandsim > nandsim.log 2>&1 || fail=1
grep -a '^ok\|^FAIL' nandsim.log
./ch341sim > ch341sim.log 2>&1 || fail=1
grep -a '^ok\|^FAIL' ch341sim.log
./norsim > norsim.log 2>&1 || fail=1
grep -a '^ok\|^FAIL' norsim.log
./ch347sim > ch347sim.log 2>&1 || fail=1
grep -a '^ok\|^FAIL' ch347sim.log
./ezpsim > ezpsim.log 2>&1 || fail=1
grep -a '^ok\|^FAIL' ezpsim.log
./spisim > spisim.log 2>&1 || fail=1
grep -a '^ok\|^FAIL' spisim.log
./usbfssim > usbfssim.log 2>&1 || fail=1
grep -a '^ok\|^FAIL' usbfssim.log
./ispsim > ispsim.log 2>&1 || fail=1
grep -a '^ok\|^FAIL' ispsim.log
./websim > websim.log 2>&1 || fail=1
grep -a '^ok\|^FAIL' websim.log
[ "$fail" = 0 ] && echo "all tests passed"
exit $fail
