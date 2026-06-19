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
ULIBDIRS := curl torrent crypto gfx zlib png mp3 sqlite clipboard mime font brotli math gc js websocket
UCFLAGS  := -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall \
           -mno-sse -mno-mmx -mfpmath=387 $(VERSION_DEF) \
           -Iuser/lib $(addprefix -Iuser/lib/,$(ULIBDIRS))

# Base every program links: crt + the syscall/libc lib. GUI (windowed) apps also
# link $(GUILIB) — gfx + the scalable font rasterizer; CLI apps don't (they just
# read/write stdio and the terminal renders their output), so they carry no
# graphics weight. gfx loads its fonts at runtime from /etc/fonts (see kernel
# install + user/lib/gfx), so no app bakes in a bitmap atlas.
FONT    := user/lib/font/font.o   # TrueType/OpenType(glyf) parser + AA glyph rasterizer
ULIB    := user/lib/start.o user/lib/lib.o
GUILIB  := user/lib/gfx/gfx.o $(FONT)

# Optional libs, linked only into the programs that use them (so a tiny program
# doesn't carry the 32 KiB inflate window or the decoder buffers).
INFLATE := user/lib/zlib/inflate.o    # zlib: streaming DEFLATE (gzip/png)
UPNG    := user/lib/png/upng.o        # libpng: streaming PNG decode (uses zlib)
URL     := user/lib/curl/url.o        # shared URL parse/resolve (curl + browser)
HTTP    := user/lib/curl/http.o       # libcurl: HTTP/1.1 over net_* (curl + torrent tracker)
HEADERS := user/lib/curl/headers.o    # libcurl: HTTP response header parser (curl + browser)
H2      := user/lib/curl/h2.o         # libcurl: HTTP/2 engine (HPACK + framing)
QUIC    := user/lib/curl/quic.o       # libcurl: QUIC v1 transport (RFC 9000/9001)
H3      := user/lib/curl/h3.o         # libcurl: HTTP/3 framing + QPACK (RFC 9114/9204)
MP3     := user/lib/mp3/mp3.o         # streaming MPEG audio decoder (.mp3/.mp2/.mp1)
LIBGCC  := $(shell $(CC) -m32 -print-libgcc-file-name)  # 64-bit div helpers etc.
CRYPTO  := user/lib/crypto/crypto.o   # SHA-1/SHA-256/x25519/AES-GCM (torrent, ssh, curl)
WEBSOCKET := user/lib/websocket/websocket.o  # RFC 6455 client (browser JS WebSocket); needs $(CRYPTO)
TORLIB  := user/lib/torrent/bencode.o user/lib/torrent/torrent.o   # libtorrent
SQLITE  := user/lib/sqlite/sqlite.o   # real SQLite3 file-format DB (browser cookie jar)
CLIPBOARD := user/lib/clipboard/clipboard.o  # clipd client (multi-item clipboard)
MIME    := user/lib/mime/mime.o       # filename/content -> MIME type helper
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
           user/lib/js/value.o user/lib/js/interp.o user/lib/js/builtins.o
# brotli: from-scratch RFC 7932 codec (HTTP `br` content-coding). decode needs the
# 122 KiB static dictionary blob (objcopy of dictionary.bin -> the path-derived
# symbol decode.c externs); encode is standalone. Links with $(LIBGCC).
BROTLIDICT := user/lib/brotli/dict_blob.o
BROTLI  := user/lib/brotli/decode.o user/lib/brotli/encode.o $(BROTLIDICT)
$(BROTLIDICT): user/lib/brotli/dictionary.bin
	objcopy -I binary -O elf32-i386 -B i386 $< $@
# woff: WOFF1 (zlib) + WOFF2 (brotli + glyf transform) container -> SFNT. Lives in
# user/lib/font and is wired into font_load via a weak ref, so font_load decodes
# web fonts transparently; only programs that link $(WOFF) drag in brotli/inflate.
WOFF    := user/lib/font/woff.o
TERMLIB := user/lib/zlib/lzss.o user/lib/zlib/deflate.o $(CRYPTO) $(CLIPBOARD)  # the shell's codecs + crypto

user/lib/start.o: user/lib/start.S
	$(CC) $(UCFLAGS) -c $< -o $@
user/lib/lib.o: user/lib/lib.c
	$(CC) $(UCFLAGS) -c $< -o $@

# Everything under user/ (the libs and the apps trees) compiles with UCFLAGS.
user/%.o: user/%.c
	$(CC) $(UCFLAGS) -c $< -o $@
