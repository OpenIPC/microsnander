#!/bin/sh
#
# tests/run.sh — main.c against tests/stub.c (no programmer needed): exit
# codes, option conflicts, -u on chips without unprotect, verify after write.
#
#   tests/run.sh            (CC=<compiler> to choose the compiler)
#
set -u
T=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
${CC:-cc} -std=gnu99 -Wall -Werror -I"$T/../src" -o "$W/ms" "$T/../src/main.c" "$T/stub.c" || exit 1
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
t 0 "-i detected"                     $B -i
t 1 "-i chip not found"               MS_CHIP=none $B -i
t 0 "-u NOR"                          $B -u
t 1 "-u NOR fails"                    MS_FAIL=unprotect $B -u
t 1 "-u NAND: not supported, no crash" MS_CHIP=nand $B -u
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
t 1 "-w missing file"                 $B -w nope.bin
t 1 "-w empty file"                   $B -w /dev/null
printf 'abc' > abc.bin
t 0 "-w file shorter than -l, -v"     $B -w abc.bin -a 0x100 -l 0x1000 -v
$B -w abc.bin -a 0x100 -l 0x1000 | grep -q 'writing 0x3 bytes' && echo "ok   only the file bytes are written" || { echo "FAIL short file length"; fail=1; }
$B -w img.bin >/dev/null
t 0 "-r full"                         $B -r out.bin
cmp -s out.bin img.bin && echo "ok   -r gives the written image" || { echo "FAIL -r content"; fail=1; }
t 1 "-r read fails"                   MS_FAIL=read $B -r out2.bin
t 1 "-r unwritable file"              $B -r /nonexistent/dir/out.bin
[ "$fail" = 0 ] && echo "all tests passed"
exit $fail
