#!/bin/sh
# aac_seek_gen.sh, generate AAC vectors (ADTS and MP4) with ffmpeg and run
# tests/media_seek_test.c: whole-stream accuracy, then seek landing and reported
# position. Needs ffmpeg on PATH.
set -e
DIR="${TMPDIR:-/tmp}/aacseek"
mkdir -p "$DIR"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
L="$ROOT/user/lib"

cc -std=gnu11 -fsanitize=address -Wno-comment -include "$ROOT/tests/media_host.h" -I"$ROOT/tests" -I$L/media -I$L/mp3 -I$L/aac -I$L/alac -I$L/mp4 \
   -I$L/ogg -I$L/math "$ROOT/tests/media_seek_test.c" $L/media/media.c $L/mp3/mp3.c $L/aac/aac.c $L/alac/alac.c \
   $L/mp4/mp4.c $L/ogg/ogg.c $L/ogg/vorbis.c $L/ogg/oggplay.c -lm -o "$DIR/media_seek_test"

gen() {
	name="$1"; ext="$2"; maxdrift="$3"
	ffmpeg -v error -y -f lavfi -i "aevalsrc=0.5*sin(2*PI*(300+200*t)*t)|0.4*sin(2*PI*(500+100*t)*t):s=48000" \
		-t 6 -c:a aac -aac_pns 0 -b:a 128k "$DIR/$name.$ext"
	ffmpeg -v error -y -i "$DIR/$name.$ext" -f s16le -acodec pcm_s16le -ac 2 "$DIR/$name.s16"
	printf '== %s ==\n' "$name"
	"$DIR/media_seek_test" "$DIR/$name.$ext" "$DIR/$name.s16" "$maxdrift"
}

gen adts aac 0
gen mp4 m4a 0
echo "all done"
