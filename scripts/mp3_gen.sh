#!/bin/sh
# mp3_gen.sh — generate MP3 vectors with lame/ffmpeg and run tests/mp3_test.c.
# Needs ffmpeg and lame on PATH.
set -e
DIR="${TMPDIR:-/tmp}/mp3test"
mkdir -p "$DIR"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cc -std=gnu11 -fsanitize=address -I"$ROOT/scripts" -I"$ROOT/user/lib/mp3" \
   "$ROOT/tests/mp3_test.c" "$ROOT/user/lib/mp3/mp3.c" -lm -o "$DIR/mp3_test"

# gen name exact lame-args... : encode a synthetic source, decode the reference.
gen() {
	name="$1"; exact="$2"; src="$3"; shift 3
	ffmpeg -v error -y -f lavfi -i "$src" -t 4 "$DIR/$name.wav"
	lame --quiet "$@" "$DIR/$name.wav" "$DIR/$name.mp3"
	af=""
	[ "$(ffprobe -v error -show_entries stream=channels -of csv=p=0 "$DIR/$name.mp3")" = 1 ] && af="pan=stereo|c0=c0|c1=c0"
	ffmpeg -v error -y -i "$DIR/$name.mp3" ${af:+-af "$af"} -f s16le -acodec pcm_s16le -ac 2 "$DIR/$name.s16"
	printf '== %s ==\n' "$name"
	"$DIR/mp3_test" "$DIR/$name.mp3" "$DIR/$name.s16" "$exact"
}

SW="aevalsrc=0.5*sin(2*PI*(300+200*t)*t)|0.4*sin(2*PI*(500+100*t)*t):s=44100"
gen cbr128 1 "$SW" -b 128
gen vbr 1 "$SW" -V2
gen mono_22k 0 "aevalsrc=0.5*sin(2*PI*(300+200*t)*t):s=22050" -m m -b 48
gen cbr_notag 0 "$SW" -b 128 -t
gen id3 1 "$SW" -b 192 --tt title --ta artist --add-id3v2
echo "all done"
