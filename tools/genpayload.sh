#!/bin/sh
# Emit the table of files the kernel installs at boot (kernel/install.h) from a list of "mode disk-path source"
# lines: tools/genpayload.sh kernel/payload.list > kernel/payload.c
# Each file's CRC is taken here, so the kernel never has to checksum its payload while booting.
set -e
list=$1

echo '#include "install.h"'
n=0
while read -r mode path src; do
	printf '__asm__(".pushsection .rodata\\n.balign 4\\npayload%d_start:\\n.incbin \\"%s\\"\\npayload%d_end:\\n.popsection");\n' \
		"$n" "$src" "$n"
	echo "extern const unsigned char payload${n}_start[], payload${n}_end[];"
	n=$((n + 1))
done <"$list"

echo 'const struct payload_file payload[] = {'
n=0
while read -r mode path src; do
	crc=$(cksum <"$src" | cut -d ' ' -f 1)
	printf '\t{"%s", payload%d_start, payload%d_end, 0x%08xu, %s},\n' "$path" "$n" "$n" "$crc" "$mode"
	n=$((n + 1))
done <"$list"
echo '};'
echo 'const unsigned payload_count = sizeof payload / sizeof payload[0];'
