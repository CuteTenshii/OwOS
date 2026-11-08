#!/bin/sh
# Emit a C table embedding the given font files: tools/genfontstore.sh fonts/*.ttf > kernel/fontstore.c
echo 'struct font_blob { const char *name; const unsigned char *start, *end; };'
n=0
for f in "$@"; do
	printf '__asm__(".section .rodata\\n.balign 4\\nfont%d_start:\\n.incbin \\"%s\\"\\nfont%d_end:\\n");\n' "$n" "$f" "$n"
	echo "extern const unsigned char font${n}_start[], font${n}_end[];"
	n=$((n + 1))
done
echo 'const struct font_blob fontstore[] = {'
n=0
for f in "$@"; do
	echo "	{\"$(basename "$f")\", font${n}_start, font${n}_end},"
	n=$((n + 1))
done
echo '};'
echo 'const unsigned fontstore_count = sizeof fontstore / sizeof fontstore[0];'
