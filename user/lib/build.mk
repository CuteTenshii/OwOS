# user/lib/build.mk — user/lib, the user-space libraries.
#
# -mno-sse/-mmx + x87 math: like the kernel, user code must avoid SSE — the
# context switch saves no FPU/SSE state, and the float->int conversions gcc would
# otherwise emit as cvttss2si (#UD on a no-SSE CPU like QEMU's qemu32) must use
# x87 instead. The user lib is one directory per actual library — curl/ torrent/
# crypto/ gfx/ zlib/ png/ mp3/ font/ — each linked independently into the programs
# that use it, and each added to the include path so the flat `#include "http.h"`
# convention keeps resolving (same trick as the kernel's DRVDIRS). libc
# (start/lib.c/owos.h) is the base every program includes, so it stays at the
# user/lib root.
ULIBDIRS := curl torrent crypto tls netconn gfx zlib png webp mp3 sqlite clipboard mime font brotli math gc js websocket notify media json protobuf alac mdns resolve stun cast mp4 mpeg h264 aac dialog base64
UCFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -O$(OPT_LEVEL) -Wall \
           -mno-sse -mno-mmx -mfpmath=387 $(VERSION_DEF) -MMD -MP \
           -Icommon -Iuser/lib $(addprefix -Iuser/lib/,$(ULIBDIRS))

# Debug build (make JSDEBUG=1 ...): the JS engine logs every thrown exception with
# its stack to the console, so swallowed failures surface. Diagnostic only.
ifdef JSDEBUG
UCFLAGS += -DJS_TRACE_THROW
endif

# Base every program links: crt + the syscall/libc lib. GUI (windowed) apps also
# link $(GUILIB) — gfx + the scalable font rasterizer; CLI apps don't (they just
# read/write stdio and the terminal renders their output), so they carry no
# graphics weight. gfx loads its fonts at runtime from /etc/fonts (see kernel
# install + user/lib/gfx), so no app bakes in a bitmap atlas.
FONT    := user/lib/font/font.o   # TrueType/OpenType(glyf) parser + AA glyph rasterizer
ULIB    := user/lib/start.o user/lib/lib.o
GUILIB  := user/lib/gfx/gfx.o user/lib/gfx/selection.o user/lib/gfx/sidebar.o user/lib/gfx/select.o user/lib/gfx/tabbar.o $(FONT)
DIALOG  := user/lib/dialog/dialog.o   # reusable modal message-box + property-sheet dialogs (uses gfx)

# Optional libs, linked only into the programs that use them (so a tiny program
# doesn't carry the 32 KiB inflate window or the decoder buffers).
INFLATE := user/lib/zlib/inflate.o    # zlib: streaming DEFLATE (gzip/png)
CRC32   := user/lib/zlib/crc32.o      # zlib: the shared CRC-32 (deflate + stun FINGERPRINT)
UPNG    := user/lib/png/upng.o        # libpng: streaming PNG decode (uses zlib)
PNGENC  := user/lib/png/pngenc.o      # PNG encode (uncompressed); screenshot path, uses $(CRC32)
WEBP    := user/lib/webp/webp.o        # WebP lossless (VP8L) decode + encode (no deps)
URL     := user/lib/curl/url.o        # shared URL parse/resolve (curl + browser)
HTTP    := user/lib/curl/http.o       # libcurl: HTTP/1.1 over net_* (curl + torrent tracker)
HEADERS := user/lib/curl/headers.o    # libcurl: HTTP response header parser (curl + browser)
HTTPCACHE := user/lib/curl/httpcache.o  # libcurl: RFC 7234 caching policy (browser cache)
H2      := user/lib/curl/h2.o         # libcurl: HTTP/2 engine (HPACK + framing)
QUIC    := user/lib/curl/quic.o       # libcurl: QUIC v1 transport (RFC 9000/9001)
H3      := user/lib/curl/h3.o         # libcurl: HTTP/3 framing + QPACK (RFC 9114/9204)
MP3     := user/lib/mp3/mp3.o         # streaming MPEG audio decoder (.mp3/.mp2/.mp1)
# mp4: ISO base media file format (.mp4/.mov/.m4v/.m4a) demuxer — box tree ->
# tracks + a flat sample table, codec-agnostic. The substrate a video decoder
# consumes; pure computation, libc-only (needs $(LIBGCC) for 64-bit box sizes).
MP4     := user/lib/mp4/mp4.o
# mpeg: from-scratch MPEG-1 video (ISO 11172-2) decoder + encoder — I/P/B pictures,
# motion compensation, DCT/IDCT, run/level VLCs. Both directions round-trip through
# ffmpeg's mpeg1video. Pure computation (x87 doubles for the IDCT), libc-only.
MPEG    := user/lib/mpeg/decode.o user/lib/mpeg/encode.o
# h264: from-scratch H.264/AVC Constrained Baseline decoder (CAVLC) — NAL/RBSP,
# SPS/PPS/slice parsing, I/P slices, intra + inter (quarter-pel) prediction, the
# 4x4 integer transform, and the in-loop deblocking filter. Verified bit-exact
# against ffmpeg/libx264 (scripts/h264_test.c). Consumes an Annex-B stream or an
# MP4 track's avcC + access units (user/lib/mp4). Pure integer computation,
# libc-only (needs $(LIBGCC) for the container path's 64-bit ops).
H264    := user/lib/h264/nal.o user/lib/h264/cavlc.o user/lib/h264/transform.o \
           user/lib/h264/intra.o user/lib/h264/inter.o user/lib/h264/deblock.o \
           user/lib/h264/slice.o user/lib/h264/h264.o
# alac: from-scratch Apple Lossless (ALAC) encoder + decoder, bit-exact with
# Apple's reference. Pure integer computation, libc-only (needs $(LIBGCC) for the
# 64-bit ops in the container/CLI). Used by the `alac` CLI and the media engine.
ALAC    := user/lib/alac/alac.o
# aac: from-scratch MPEG-2/4 AAC Low Complexity decoder — the audio of .m4a/.mp4
# and ADTS .aac. Full LC tool set (M/S, intensity, PNS, TNS, long/short IMDCT),
# verified bit-exact against ffmpeg (scripts/aac_test.c). Pure computation, needs
# $(MATH) (pow/sqrt/trig for dequant + windows) + $(LIBGCC).
AAC     := user/lib/aac/aac.o
# media: file decoder (WAV/MPEG/ALAC/AAC -> 48 kHz/S16/stereo, seekable) + a
# software mixer thread that blends several sources into the one kernel PCM
# stream. Used by the media player and the browser's <audio>/<video>. Needs
# $(MP3) + $(ALAC) + $(AAC) + $(LIBGCC), and $(MATH) (AAC's dequant/windows use
# pow/sqrt/trig). Its .m4a/.mp4 path also calls the MP4 demuxer, so a consumer
# that plays AAC-in-MP4 must link $(MP4) + $(MATH) too (both media consumers
# already do via $(VIDEO) / their own $(MATH)).
MEDIA   := user/lib/media/media.o $(MP3) $(ALAC) $(AAC)
# video: the media engine's H.264 picture path (media.h media_video_*). A thin
# front end over $(H264) + $(MP4) that decodes a local .264/.mp4 to timed RGB
# frames. Linked only by video consumers so audio-only programs skip the decoder.
VIDEO   := user/lib/media/video.o $(H264) $(MP4)
LIBGCC  := $(shell $(UCC) -m32 -print-libgcc-file-name)  # 64-bit div helpers etc.
CRYPTO  := user/lib/crypto/crypto.o user/lib/crypto/ed25519.o   # SHA-1/256/512/HMAC/PRF/HKDF/AES-GCM/X25519 + Ed25519 (torrent, ssh, curl)
# tls: user-space TLS 1.3 client with a TLS 1.2 fallback, over an owned SK_TCP
# socket. Ported from kernel/tls.c; cert not verified. Needs $(CRYPTO).
TLS     := user/lib/tls/tls.o
# netconn: a TCP-or-TLS byte-stream transport wrapper (SK_TCP socket or $(TLS)
# conn behind one handle). Used by libcurl HTTP/1.1 + the WebSocket client; needs
# $(TLS) (+ $(CRYPTO), which TLS needs).
NETCONN := user/lib/netconn/netconn.o
WEBSOCKET := user/lib/websocket/websocket.o  # RFC 6455 client (browser JS WebSocket); needs $(CRYPTO)
TORLIB  := user/lib/torrent/bencode.o user/lib/torrent/torrent.o   # libtorrent
SQLITE  := user/lib/sqlite/sqlite.o   # real SQLite3 file-format DB (browser cookie jar)
CLIPBOARD := user/lib/clipboard/clipboard.o  # clipd client (multi-item clipboard)
MIME    := user/lib/mime/mime.o       # filename/content -> MIME type helper
# base64: RFC 4648 encode/decode. Allocation-free, pure computation, libc-only.
BASE64  := user/lib/base64/base64.o
# json: standalone RFC 8259 value tree (parse + builder + encode). Pure
# computation, no syscalls; needs $(MATH) for dtoa/strtod and $(LIBGCC).
JSON    := user/lib/json/json.o
# protobuf: schema-agnostic Protocol Buffers wire-format codec (streaming reader
# + growable writer). Pure computation, libc-only (needs $(LIBGCC) for 64-bit
# varint shifts/divides). Consumers supply their own schema (Chromecast, Google).
PROTOBUF := user/lib/protobuf/protobuf.o
# cast: Google Cast (Chromecast) sender — mDNS discovery + a TLS channel speaking
# length-prefixed CastMessage protobufs with JSON payloads (Default Media
# Receiver control). Bare .o; a consumer also links $(PROTOBUF) $(JSON) $(MATH)
# $(NETCONN) $(TLS) $(CRYPTO) $(RESOLVE) (which bundles $(MDNS)).
CAST    := user/lib/cast/cast.o
NOTIFY  := user/lib/notify/notify.o   # notifyd client (desktop notifications)
# mdns: mDNS (RFC 6762) + DNS-SD (RFC 6763) client over a UDP socket — .local
# resolution + service discovery. Pure over net_* + libc; needs $(LIBGCC).
MDNS    := user/lib/mdns/mdns.o
# resolve: the getaddrinfo analog — hostname -> IPv4 (IP literal / .local via
# $(MDNS) / unicast DNS from /etc/resolv.conf). Now that the kernel connect path
# takes an IP only, this is the user-space resolver; needs $(MDNS) + $(LIBGCC).
RESOLVE := user/lib/resolve/resolve.o $(MDNS)
# stun: STUN (RFC 5389/8489) message codec + server-reflexive query — the generic
# NAT-traversal substrate ICE/WebRTC gathers candidates over. Needs $(CRYPTO)
# (hmac_sha1 for MESSAGE-INTEGRITY) and $(RESOLVE) (stun_query's name->IP).
STUN    := user/lib/stun/stun.o
# math: freestanding x87 libm + shortest-round-trip number<->string (Dragon4).
# Pure computation, no syscalls; links with $(LIBGCC) for 64-bit integer divides.
MATH    := user/lib/math/math.o user/lib/math/fmt.o
# gc: precise mark-sweep collector over the malloc heap (roots + tracer supplied
# by the client). For language runtimes that need managed memory.
GC      := user/lib/gc/gc.o
# js: the JavaScript engine for the browser. Front end (lexer/parser/AST) +
# tree-walk interpreter (values/objects over $(GC), scopes/closures, core
# stdlib). Pure computation; links with $(MATH) (strtod/dtoa), $(GC) and
# $(LIBGCC). Class evaluation, RegExp matching, async/modules come next.
JS      := user/lib/js/lexer.o user/lib/js/parser.o user/lib/js/ast.o \
           user/lib/js/value.o user/lib/js/interp.o user/lib/js/builtins.o \
           user/lib/js/bigint.o
# brotli: from-scratch RFC 7932 codec (HTTP `br` content-coding). decode needs the
# 122 KiB static dictionary blob (objcopy of dictionary.bin -> the path-derived
# symbol decode.c externs); encode is standalone. Links with $(LIBGCC).
BROTLIDICT := user/lib/brotli/dict_blob.o
BROTLI  := user/lib/brotli/decode.o user/lib/brotli/encode.o $(BROTLIDICT)
$(BROTLIDICT): user/lib/brotli/dictionary.bin
	$(UOBJCOPY) -I binary -O elf32-i386 -B i386 $< $@
# woff: WOFF1 (zlib) + WOFF2 (brotli + glyf transform) container -> SFNT. Lives in
# user/lib/font and is wired into font_load via a weak ref, so font_load decodes
# web fonts transparently; only programs that link $(WOFF) drag in brotli/inflate.
WOFF    := user/lib/font/woff.o
TERMLIB := user/lib/zlib/lzss.o user/lib/zlib/deflate.o $(CRC32) $(CRYPTO) $(CLIPBOARD)  # the shell's codecs + crypto

user/lib/start.o: user/lib/start.S $(ARCH_STAMP)
	$(UCC) $(UCFLAGS) -c $< -o $@
user/lib/lib.o: user/lib/lib.c $(ARCH_STAMP)
	$(UCC) $(UCFLAGS) -c $< -o $@

# Everything under user/ (the libs and the apps trees) compiles with UCFLAGS.
user/%.o: user/%.c $(ARCH_STAMP)
	$(UCC) $(UCFLAGS) -c $< -o $@

# Header dependencies: -MMD -MP (above) emits a .d beside each .o listing the
# headers it includes, so editing a header (e.g. browser.h's struct layout)
# recompiles every .o that includes it — no stale object with a mismatched ABI.
-include $(shell find user -name '*.d' 2>/dev/null)
