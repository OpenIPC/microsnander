#!/bin/sh
#
# web_fonts.sh <font.woff2>... — the page's fonts (brass 1.5.0 of OpenIPC:
# Brass Serif and Brass Mono, fonts/OFL.txt) as a C header of arrays, made at
# build time and never committed, as web_page.h: web.c serves them at
# /fonts/<file>.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
set -eu
[ $# -gt 0 ] || { echo "web_fonts.sh: no fonts" >&2; exit 1; }
for f in "$@"; do
	[ -s "$f" ] && [ -r "$f" ] || { echo "web_fonts.sh: cannot read $f" >&2; exit 1; }
done
echo '/* made by web_fonts.sh from src/fonts at build time: do not edit */'
n=0
for f in "$@"; do
	printf 'static const unsigned char web_font%d[] = {\n' "$n"
	od -An -v -tu1 "$f" | awk '{ for (i = 1; i <= NF; i++) { printf "%s,", $i; if (++n % 16 == 0) printf "\n" } }
		END { if (n % 16) printf "\n" }'
	echo '};'
	n=$((n + 1))
done
echo 'static const struct { const char *name; const unsigned char *data; unsigned long len; } web_fonts[] = {'
n=0
for f in "$@"; do
	printf '\t{ "%s", web_font%d, sizeof(web_font%d) },\n' "$(basename "$f")" "$n" "$n"
	n=$((n + 1))
done
echo '};'
