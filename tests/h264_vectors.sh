#!/bin/sh
# Decode a set of libx264 streams with the host build of user/lib/h264 and
# require every frame to match ffmpeg's decode bit for bit.
# Needs ffmpeg with libx264; run from the repository root.
set -e
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -std=c11 -Itests -Iuser/lib/h264 tests/h264_test.c user/lib/h264/*.c -lm -o "$tmp/h264_test"

fails=0
# name, geometry, source, encoder options
run() {
  name=$1 geo=$2 src=$3
  shift 3
  w=${geo%x*}
  h=${geo#*x}
  ffmpeg -v error -y -f lavfi -i "$src=size=$geo:rate=25" -t 1.2 -c:v libx264 -pix_fmt yuv420p "$@" \
    -bsf:v h264_mp4toannexb -f h264 "$tmp/$name.264"
  ffmpeg -v error -y -i "$tmp/$name.264" -f rawvideo -pix_fmt yuv420p "$tmp/$name.yuv"
  if "$tmp/h264_test" "$tmp/$name.264" "$tmp/$name.yuv" "$w" "$h" >"$tmp/$name.log" 2>&1; then
    echo "ok   $name"
  else
    echo "FAIL $name"
    tail -3 "$tmp/$name.log"
    fails=$((fails + 1))
  fi
}

T=testsrc2
run intra-cavlc 176x144 testsrc -profile:v baseline -g 1
run intra-cabac 176x144 testsrc -profile:v main -g 1
run p-cavlc 176x144 $T -profile:v baseline -weightp 0 -refs 3 -g 15
run p-cabac 176x144 $T -profile:v main -bf 0 -weightp 0 -refs 3 -g 15
run p-wrap 176x144 $T -profile:v main -bf 0 -weightp 0 -refs 3 -g 40
run p-weighted 176x144 $T -profile:v main -bf 0 -weightp 2 -g 15
run p-slices 176x144 $T -profile:v main -bf 0 -g 15 -x264-params slices=4
run high-8x8-cabac 176x144 $T -profile:v high -bf 0 -g 15
run high-8x8-cavlc 176x144 $T -profile:v high -bf 0 -coder 0 -g 15
run high-cqm 176x144 $T -profile:v high -bf 3 -g 30 -x264-params cqm=jvt
run b-spatial 176x144 $T -profile:v main -bf 2 -weightp 0 -g 15
run b-temporal 176x144 $T -profile:v high -bf 3 -g 30 -x264-params direct=temporal
run b-cavlc 176x144 $T -profile:v high -bf 3 -coder 0 -g 30 -x264-params direct=temporal:ref=4
run b-pyramid 176x144 $T -profile:v high -bf 4 -b-pyramid strict -refs 4 -g 40 -x264-params weightb=1
run hd-mandelbrot 320x240 mandelbrot -profile:v high -bf 3 -g 50 -x264-params ref=5:subme=9:me=umh:trellis=2

[ "$fails" = 0 ]
