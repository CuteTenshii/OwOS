# user/lib/build.mk - user/lib, the user-space libraries.
#
# The user lib is one directory per actual library - curl/ torrent/ crypto/ gfx/
# zlib/ png/ mp3/ font/ - each linked independently into the programs that use it,
# and each added to the include path so the flat `#include "http.h"` convention
# keeps resolving (same trick as the kernel's DRVDIRS). libc (start/lib.c/owos.h)
# is the base every program includes, so it stays at the user/lib root.
#
# i386: -mno-sse/-mmx + x87 math. The 32-bit kernel saves only the x87 file on a
# context switch, and QEMU's qemu32 CPU has no SSE (cvttss2si would #UD).
# x86_64: the ABI passes floats in SSE registers, and the 64-bit kernel enables SSE
# and saves the full FXSAVE image per task.
ULIBDIRS := $(patsubst user/lib/%/,%,$(wildcard user/lib/*/))
ifeq ($(ARCH),x86_64)
  UBITS     := 64
  UARCHOPTS :=
else
  UBITS     := 32
  UARCHOPTS := -mno-sse -mno-mmx -mfpmath=387
endif
UCFLAGS := $(UARCHFLAGS) -ffreestanding -fno-pie -fno-stack-protector -O$(OPT_LEVEL) -Wall \
           $(UARCHOPTS) $(VERSION_DEF) -MMD -MP \
           -Icommon -Iuser/lib $(addprefix -Iuser/lib/,$(ULIBDIRS))

# Debug build (make JSDEBUG=1 ...): the JS engine logs every thrown exception with
# its stack to the console, so swallowed failures surface. Diagnostic only.
ifdef JSDEBUG
UCFLAGS += -DJS_TRACE_THROW
endif

# Base every program links: crt + the syscall/libc lib. GUI (windowed) apps also
# link $(GUILIB) - gfx + the scalable font rasterizer; CLI apps don't (they just
# read/write stdio and the terminal renders their output), so they carry no
# graphics weight. gfx loads its fonts at runtime from /etc/fonts (see kernel
# install + user/lib/gfx), so no app bakes in a bitmap atlas.
FONT     = user/lib/font/font.o user/lib/font/fontstore.o   # TrueType/OpenType(glyf) parser + AA glyph rasterizer
ULIB     = user/lib/start_$(UBITS).o user/lib/lib.o user/lib/opt.o user/lib/mutex.o user/lib/win/win.o
GUILIB   = user/lib/gfx/gfx.o user/lib/gfx/style.o user/lib/gfx/focus.o user/lib/gfx/a11y.o user/lib/gfx/selection.o user/lib/gfx/textfield.o user/lib/gfx/sidebar.o user/lib/gfx/select.o user/lib/gfx/tabbar.o user/lib/gfx/scrollview.o user/lib/gfx/toggle.o $(FONT)
FILEPICK  = user/lib/gfx/filelist.o user/lib/gfx/filepicker.o $(GUILIB)   # modal file/folder picker (uses gfx widgets)
DIALOG   = user/lib/dialog/dialog.o $(GUILIB)   # reusable modal message-box + property-sheet dialogs (uses gfx)

# Optional libs, linked only into the programs that use them (so a tiny program
# doesn't carry the 32 KiB inflate window or the decoder buffers). Each one names the
# libraries it calls, so an app lists only what its own code uses.
INFLATE  = user/lib/zlib/inflate.o    # zlib: streaming DEFLATE (gzip/png)
DEFLATE  = user/lib/zlib/deflate.o $(CRC32)   # zlib: gzip/zlib/raw DEFLATE encoder
CRC32    = user/lib/zlib/crc32.o      # zlib: the shared CRC-32 (deflate + stun FINGERPRINT)
JPEG     = user/lib/jpeg/jpeg.o        # baseline + progressive JPEG decode (no deps)
PNGDEC   = user/lib/png/png.o $(INFLATE)   # libpng: streaming PNG decode
PNGENC   = user/lib/png/pngenc.o $(CRC32)   # PNG encode (uncompressed); screenshot path
WEBP     = user/lib/webp/webp.o user/lib/webp/vp8.o  # WebP: lossless (VP8L) decode + encode, lossy (VP8) decode (no deps)
GIF      = user/lib/gif/gif.o          # GIF87a/89a decode with frame compositing (no deps)
IMGANIM  = user/lib/imganim/imganim.o $(GIF) $(WEBP)   # frame player for GIF and animated WebP
CANVAS2D  = user/lib/canvas2d/canvas2d.o user/lib/canvas2d/path.o user/lib/canvas2d/raster.o user/lib/canvas2d/stroke.o user/lib/canvas2d/blend.o user/lib/canvas2d/text.o $(CRC32) $(FONT) $(MATH)   # software 2D canvas rasterizer
URL      = user/lib/curl/url.o        # shared URL parse/resolve (curl + browser)
HTTP     = user/lib/curl/http.o $(H2) $(H3) $(HEADERS) $(NETCONN) $(URL)   # libcurl: HTTP/1.1 over net_* (curl + torrent tracker)
HEADERS  = user/lib/curl/headers.o    # libcurl: HTTP response header parser (curl + browser)
HTTPCACHE  = user/lib/curl/httpcache.o $(HEADERS)   # libcurl: RFC 7234 caching policy (browser cache)
H2       = user/lib/curl/h2.o $(NETCONN)   # libcurl: HTTP/2 engine (HPACK + framing)
QUIC     = user/lib/curl/quic.o $(CRYPTO) $(RESOLVE) $(TLS)   # libcurl: QUIC v1 transport (RFC 9000/9001)
H3       = user/lib/curl/h3.o $(NETCONN) $(QUIC)   # libcurl: HTTP/3 framing + QPACK (RFC 9114/9204)
MP3      = user/lib/mp3/mp3.o         # streaming MPEG audio decoder (.mp3/.mp2/.mp1)
# mp4: ISO base media file format (.mp4/.mov/.m4v/.m4a) demuxer - box tree ->
# tracks + a flat sample table, codec-agnostic. The substrate a video decoder
# consumes; pure computation, libc-only (needs $(LIBGCC) for 64-bit box sizes).
MP4      = user/lib/mp4/mp4.o
# mpeg: from-scratch MPEG-1 video (ISO 11172-2) decoder + encoder - I/P/B pictures,
# motion compensation, DCT/IDCT, run/level VLCs. Both directions round-trip through
# ffmpeg's mpeg1video. Pure computation (x87 doubles for the IDCT), libc-only.
MPEG     = user/lib/mpeg/decode.o user/lib/mpeg/encode.o
# h264: from-scratch H.264/AVC decoder (Baseline/Main/High, CAVLC + CABAC) - NAL/RBSP,
# SPS/PPS/slice parsing, I/P/B slices, intra + inter (quarter-pel, weighted)
# prediction, 4x4/8x8 integer transforms, the in-loop deblocking filter and DPB
# reordering. Verified bit-exact against ffmpeg/libx264 (tests/h264_test.c).
# Consumes an Annex-B stream or an MP4 track's avcC + access units
# (user/lib/mp4). Pure integer computation, libc-only (needs $(LIBGCC) for the
# container path's 64-bit ops).
H264     = user/lib/h264/nal.o user/lib/h264/cavlc.o user/lib/h264/cabac.o user/lib/h264/transform.o \
           user/lib/h264/intra.o user/lib/h264/inter.o user/lib/h264/deblock.o \
           user/lib/h264/slice.o user/lib/h264/h264.o
# alac: from-scratch Apple Lossless (ALAC) encoder + decoder, bit-exact with
# Apple's reference. Pure integer computation, libc-only (needs $(LIBGCC) for the
# 64-bit ops in the container/CLI). Used by the `alac` CLI and the media engine.
ALAC     = user/lib/alac/alac.o
# aac: from-scratch MPEG-2/4 AAC Low Complexity decoder - the audio of .m4a/.mp4
# and ADTS .aac. Full LC tool set (M/S, intensity, PNS, TNS, long/short IMDCT),
# verified bit-exact against ffmpeg (tests/aac_test.c). Pure computation, needs
# $(LIBGCC).
AAC      = user/lib/aac/aac.o $(MATH)
# ogg: Ogg container + from-scratch Vorbis I decoder (FFT IMDCT), checked against
# libvorbis (tests/ogg_gen.sh); oggplay.o is the streaming file reader the media
# engine drives.
OGG      = user/lib/ogg/ogg.o user/lib/ogg/vorbis.o user/lib/ogg/oggplay.o $(MATH)
# media: file decoder (WAV/MPEG/ALAC/AAC/Vorbis -> 48 kHz/S16/stereo, seekable) + a
# software mixer thread that blends several sources into the one kernel PCM
# stream. Used by the media player and the browser's <audio>/<video>; its .m4a/.mp4
# path goes through the MP4 demuxer.
MEDIA    = user/lib/media/media.o user/lib/media/tags.o $(MP3) $(ALAC) $(AAC) $(OGG) $(MATH) $(MP4)
# video: the media engine's H.264 picture path (media.h media_video_*). A thin
# front end over $(H264) + $(MP4) that decodes a local .264/.mp4 to timed RGB
# frames. Linked only by video consumers so audio-only programs skip the decoder.
VIDEO    = user/lib/media/video.o $(H264) $(MP4)
LIBGCC  := $(shell $(UCC) $(UARCHFLAGS) -print-libgcc-file-name)  # 64-bit div helpers etc.
CRYPTO   = user/lib/crypto/crypto.o user/lib/crypto/ed25519.o   # SHA-1/256/512/HMAC/PRF/HKDF/AES-GCM/X25519 + Ed25519 (torrent, ssh, curl)
# tls: user-space TLS 1.3 client with a TLS 1.2 fallback, over an owned SK_TCP
# socket. Ported from kernel/tls.c; certificates verified with $(PKI) (warn and
# allow, see tls_verify).
PKI      = user/lib/pki/bn.o user/lib/pki/rsa.o user/lib/pki/ecdsa.o user/lib/pki/x509.o $(CRYPTO)   # X.509 + RSA/ECDSA verify
TLS      = user/lib/tls/tls.o user/lib/tls/certs.o user/lib/tls/ech.o $(PKI) $(CRYPTO) $(RESOLVE)
# netconn: a TCP-or-TLS byte-stream transport wrapper (SK_TCP socket or $(TLS)
# conn behind one handle). Used by libcurl HTTP/1.1 + the WebSocket client.
NETCONN  = user/lib/netconn/netconn.o $(RESOLVE) $(TLS)
WEBSOCKET  = user/lib/websocket/websocket.o $(BASE64) $(CRYPTO) $(NETCONN)   # RFC 6455 client (browser JS WebSocket)
TORLIB   = user/lib/torrent/bencode.o user/lib/torrent/torrent.o $(CRYPTO)   # libtorrent
SQLITE   = user/lib/sqlite/sqlite.o   # real SQLite3 file-format DB (browser cookie jar)
CLIPBOARD  = user/lib/clipboard/clipboard.o  # clipd client (multi-item clipboard)
MIME     = user/lib/mime/mime.o       # filename/content -> MIME type helper
MAGIC    = user/lib/mime/magic.o      # file(1) content identification
# base64: RFC 4648 encode/decode. Allocation-free, pure computation, libc-only.
BASE64   = user/lib/base64/base64.o
# json: standalone RFC 8259 value tree (parse + builder + encode). Pure
# computation, no syscalls; needs $(LIBGCC).
JSON     = user/lib/json/json.o $(MATH)
# protobuf: schema-agnostic Protocol Buffers wire-format codec (streaming reader
# + growable writer). Pure computation, libc-only (needs $(LIBGCC) for 64-bit
# varint shifts/divides). Consumers supply their own schema (Chromecast, Google).
PROTOBUF  = user/lib/protobuf/protobuf.o
# cast: Google Cast (Chromecast) sender - mDNS discovery + a TLS channel speaking
# length-prefixed CastMessage protobufs with JSON payloads (Default Media
# Receiver control).
CAST     = user/lib/cast/cast.o $(JSON) $(MDNS) $(NETCONN) $(PROTOBUF)
# uuid: RFC 9562 UUIDs (v4, v5, v7), parse and format.
UUID     = user/lib/uuid/uuid.o user/lib/uuid/gen.o $(CRYPTO)
# plist: Apple property lists (XML and binary) as a value tree.
PLIST    = user/lib/plist/plist.o $(BASE64) $(MATH)
# airplay: AirPlay 1 video sender - mDNS discovery + plain HTTP on port 7000 (/play, /rate, /scrub).
AIRPLAY  = user/lib/airplay/airplay.o $(MDNS) $(NETCONN) $(PLIST) $(UUID)
# pdf: PDF reader + page renderer onto a canvas2d surface.
PDF      = user/lib/pdf/pdf.o user/lib/pdf/pdffilter.o user/lib/pdf/pdffunc.o user/lib/pdf/pdffont.o user/lib/pdf/pdfrender.o user/lib/pdf/pdftext.o user/lib/pdf/pdfimage.o $(CANVAS2D) $(FONT) $(INFLATE) $(JPEG) $(MATH)
NOTIFY   = user/lib/notify/notify.o   # notifyd client (desktop notifications)
TRAY     = user/lib/tray/tray.o       # compositor system tray client
# mediactl: client for the mediad media session service; session.o is the shared codec + policy.
MEDIACTL  = user/lib/mediactl/mediactl.o user/lib/mediactl/session.o
# desktop: freedesktop .desktop entry parser, Exec expansion and directory scan
DESKTOP  = user/lib/desktop/desktop.o
# rar: from-scratch RAR 3/4/5 reader (LZ, PPMd, filters, AES, BLAKE2sp)
RAR      = user/lib/rar/rar.o user/lib/rar/rar_hdr.o user/lib/rar/rar_bits.o user/lib/rar/rar_win.o user/lib/rar/rar_filter.o user/lib/rar/rar_crypto.o user/lib/rar/rar_blake2.o user/lib/rar/rar5.o user/lib/rar/rar4.o user/lib/rar/ppmd.o $(CRC32) $(CRYPTO)
# mdns: mDNS (RFC 6762) + DNS-SD (RFC 6763) client over a UDP socket - .local
# resolution + service discovery. Pure over net_* + libc; needs $(LIBGCC).
MDNS     = user/lib/mdns/mdns.o
# resolve: the getaddrinfo analog - hostname -> IPv4 (IP literal / .local via
# $(MDNS) / unicast DNS from /etc/resolv.conf). Now that the kernel connect path
# takes an IP only, this is the user-space resolver; needs $(LIBGCC).
RESOLVE  = user/lib/resolve/resolve.o user/lib/resolve/svcb.o $(MDNS)
# stun: STUN (RFC 5389/8489) message codec + server-reflexive query - the generic
# NAT-traversal substrate ICE/WebRTC gathers candidates over.
STUN     = user/lib/stun/stun.o $(CRC32) $(CRYPTO) $(RESOLVE)
# xml: allocation-free XML element reader + escaper (SOAP, device descriptions); no dependencies.
XML      = user/lib/xml/xml.o
# xmltree: XML decoder and encoder over a heap node tree (namespaces, entities, CDATA, pretty printing); libc only.
XMLTREE  = user/lib/xml/xmltree.o
# upnp: UPnP IGD client - SSDP discovery, device description, SOAP port mapping over the HTTP client.
UPNP     = user/lib/upnp/upnp.o user/lib/upnp/igd.o $(HEADERS) $(HTTP) $(URL) $(XML)
# ntp: NTPv4 client arithmetic (timestamps, packet validation, offset/delay, sample
# filter, poll schedule). Pure computation, libc-free; needs $(LIBGCC) for 64-bit divides.
NTP      = user/lib/ntp/ntp.o
# tz: built-in IANA zone table (POSIX rules), UTC<->local conversion, /etc/timezone. Pure computation.
TZ       = user/lib/tz/tz.o

# math: freestanding x87 libm + shortest-round-trip number<->string (Dragon4).
# Pure computation, no syscalls; links with $(LIBGCC) for 64-bit integer divides.
MATH     = user/lib/math/math.o user/lib/math/fmt.o
# gc: precise mark-sweep collector over the malloc heap (roots + tracer supplied
# by the client). For language runtimes that need managed memory.
GC       = user/lib/gc/gc.o
# js: the JavaScript engine for the browser. Front end (lexer/parser/AST) +
# tree-walk interpreter (values/objects over $(GC), scopes/closures, core
# stdlib). Pure computation; needs $(LIBGCC). Class evaluation, RegExp
# matching, async/modules come next.
JS       = user/lib/js/lexer.o user/lib/js/parser.o user/lib/js/ast.o \
           user/lib/js/value.o user/lib/js/interp.o user/lib/js/builtins.o \
           user/lib/js/bigint.o user/lib/js/wasm_js.o \
           user/lib/wasm/wasm.o user/lib/wasm/wasm_run.o user/lib/wasm/wasm_simd.o $(BASE64) $(GC) $(MATH)
# brotli: from-scratch RFC 7932 codec (HTTP `br` content-coding). decode needs the
# 122 KiB static dictionary blob (objcopy of dictionary.bin -> the path-derived
# symbol decode.c externs); encode is standalone. Links with $(LIBGCC).
BROTLIDICT  = user/lib/brotli/dict_blob.o
BROTLI   = user/lib/brotli/decode.o user/lib/brotli/encode.o $(BROTLIDICT)
$(BROTLIDICT): user/lib/brotli/dictionary.bin $(ARCH_STAMP)
	$(UOBJCOPY) -I binary -O $(KELF) -B $(KBFD) $< $@
# woff: WOFF1 (zlib) + WOFF2 (brotli + glyf transform) container -> SFNT. Lives in
# user/lib/font and is wired into font_load via a weak ref, so font_load decodes
# web fonts transparently; only programs that link $(WOFF) drag in brotli/inflate.
WOFF     = user/lib/font/woff.o $(BROTLI) $(INFLATE)
# zstd: from-scratch RFC 8878 codec (decode + encode), pure computation.
ZSTD     = user/lib/zstd/zstd_common.o user/lib/zstd/fse.o user/lib/zstd/huf.o user/lib/zstd/zstd_dec.o user/lib/zstd/zstd_enc.o
# archive: TAR (raw/gz/zst/br) + ZIP reader.
ARCHIVE  = user/lib/archive/archive.o user/lib/archive/zip.o $(BROTLI) $(CRC32) $(INFLATE) $(ZSTD)
SHLIB  = user/lib/zlib/lzss.o user/lib/zlib/deflate.o $(CRC32) $(CRYPTO) $(CLIPBOARD)  # the shell's codecs + crypto

user/lib/start_$(UBITS).o: user/lib/start_$(UBITS).S $(ARCH_STAMP)
	$(UCC) $(UCFLAGS) -c $< -o $@

# Everything under user/ (the libs and the apps trees) compiles with UCFLAGS.
user/%.o: user/%.c $(ARCH_STAMP)
	$(UCC) $(UCFLAGS) -c $< -o $@

# Header dependencies: -MMD -MP (above) emits a .d beside each .o listing the
# headers it includes, so editing a header (e.g. browser.h's struct layout)
# recompiles every .o that includes it - no stale object with a mismatched ABI.
-include $(shell find user -name '*.d' 2>/dev/null)
