#!/bin/sh
# aac_gen.sh — generate AAC-LC test vectors with ffmpeg and run tests/aac_test.
# Encodes synthetic signals to ADTS AAC, decodes a reference with ffmpeg, then
# compares our decoder against it. Needs ffmpeg on PATH.
set -e
DIR="${TMPDIR:-/tmp}/aactest"
mkdir -p "$DIR"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cc -std=c11 -Itests -Iuser/lib/math -Iuser/lib/aac \
   "$ROOT/tests/aac_test.c" "$ROOT/user/lib/aac/aac.c" -lm -o "$DIR/aac_test"

# -aac_pns 0 keeps the comparison deterministic: perceptual noise substitution
# fills bands with a decoder-private random sequence, so a PNS band never matches
# byte-for-byte across decoders (ffmpeg vs faad2 disagree by ~17 dB on pink noise;
# our LCG matches ffmpeg's, landing ~49 dB — but still not exact). Disabling it
# makes every band real spectral data so a correct decode is essentially exact.
gen() {
	name="$1"; rate="$2"; dur="$3"; br="$4"; shift 4
	src="$*"
	ffmpeg -v error -y -f lavfi -i "$src" -t "$dur" -ar "$rate" "$DIR/$name.wav"
	ffmpeg -v error -y -i "$DIR/$name.wav" -c:a aac -aac_pns 0 -b:a "$br" -f adts "$DIR/$name.aac"
	ffmpeg -v error -y -i "$DIR/$name.aac" -f s16le -acodec pcm_s16le "$DIR/$name.s16"
	printf '== %s (%s Hz, %s) ==\n' "$name" "$rate" "$br"
	"$DIR/aac_test" "$DIR/$name.aac" "$DIR/$name.s16"
}

# mono tone (SCE, long windows)
gen mono 44100 2 96k "sine=frequency=440:sample_rate=44100"
# stereo, distinct channels (CPE + M/S / intensity stereo)
gen stereo 44100 2 128k "aevalsrc=sin(440*2*PI*t)|sin(660*2*PI*t):s=44100"
# swept tone + transients (window switching to EIGHT_SHORT + TNS)
gen sweep 48000 3 160k "sine=frequency=200:beep_factor=4:sample_rate=48000"
# broadband pink noise (exercises the high codebooks + the cb11 escape sequence)
gen noise 32000 2 128k "anoisesrc=color=pink:sample_rate=32000:amplitude=0.5"
echo "all done"
