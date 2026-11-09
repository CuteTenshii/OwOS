#!/bin/sh
# ogg_gen.sh, generate Ogg Vorbis vectors with ffmpeg and run tests/oggplay_test.c.
# The reference decode comes from libvorbis through VLC (ffmpeg's own Vorbis
# decoder does not match libvorbis on ffmpeg-encoded stereo). VLC drops a run of leading
# frames, which the test realigns. Needs ffmpeg and VLC.
set -e
VLC="${VLC:-/Applications/VLC.app/Contents/MacOS/VLC}"
[ -x "$VLC" ] || { echo "skip: VLC not found (set VLC=...)"; exit 0; }
DIR="${TMPDIR:-/tmp}/oggtest"
mkdir -p "$DIR"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cc -std=gnu11 -fsanitize=address -Wno-comment -I"$ROOT/scripts" -I"$ROOT/user/lib/ogg" -I"$ROOT/user/lib/math" \
   "$ROOT/tests/oggplay_test.c" "$ROOT/user/lib/ogg/ogg.c" "$ROOT/user/lib/ogg/vorbis.c" \
   "$ROOT/user/lib/ogg/oggplay.c" -lm -o "$DIR/oggplay_test"

gen() {
	name="$1"; src="$2"
	ffmpeg -v error -y -f lavfi -i "$src" -t 4 -c:a vorbis -strict -2 -ac 2 "$DIR/$name.ogg"
	"$VLC" -I dummy --no-video --quiet "$DIR/$name.ogg" \
		--sout "#transcode{acodec=s16l,channels=2}:std{access=file,mux=wav,dst=$DIR/$name.wav}" vlc://quit
	ffmpeg -v error -y -i "$DIR/$name.wav" -f s16le "$DIR/$name.s16"
	printf '== %s ==\n' "$name"
	"$DIR/oggplay_test" "$DIR/$name.ogg" "$DIR/$name.s16"
}

gen stereo "aevalsrc=0.5*sin(2*PI*(300+200*t)*t)|0.4*sin(2*PI*(500+100*t)*t):s=44100"
gen noise "anoisesrc=color=pink:sample_rate=48000:amplitude=0.5"
gen pulses "aevalsrc=0.6*sin(2*PI*440*t)*lt(mod(t\,0.25)\,0.03)|0.6*sin(2*PI*660*t)*lt(mod(t\,0.4)\,0.05):s=32000"
echo "all done"
