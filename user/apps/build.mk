# user/apps/build.mk — the user programs under user/apps/{gui,cli}/, and embedding each as
# a kernel blob.
#
# GUI (windowed) apps link gfx + the font lib ($(GUILIB)); CLI apps link only the
# base ULIB. Each program ELF is wrapped into an object whose symbol is normalised
# to _binary_app_<name>_{start,end} so the kernel installer doesn't care which
# apps/ subdir a program lives in.

# Generic links. CLI = base only; GUI = base + gfx + font.
user/apps/cli/%.elf: user/apps/cli/%.o $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ $< $(ULIB) $(LIBGCC)
user/apps/gui/%.elf: user/apps/gui/%.o $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ $< $(GUILIB) $(ULIB) $(LIBGCC)

# --- GUI apps that pull in extra libs ---------------------------------------
# the terminal is a self-contained shell: zip/gzip file codecs + crypto (su/login).
user/apps/gui/term.elf: user/apps/gui/term.o $(TERMLIB) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/gui/term.o $(TERMLIB) $(GUILIB) $(ULIB) $(LIBGCC)

# the editor + cliphist use the clipboard (Cut/Copy/Paste) + the MIME text check;
# the editor also uses the dialog lib (unsaved-changes confirm).
user/apps/gui/editor.elf: user/apps/gui/editor.o $(CLIPBOARD) $(MIME) $(DIALOG) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/gui/editor.o $(CLIPBOARD) $(MIME) $(DIALOG) $(GUILIB) $(ULIB) $(LIBGCC)
user/apps/gui/cliphist.elf: user/apps/gui/cliphist.o $(CLIPBOARD) $(MIME) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/gui/cliphist.o $(CLIPBOARD) $(MIME) $(GUILIB) $(ULIB) $(LIBGCC)

# files uses the dialog lib for delete confirmation + a Properties sheet.
user/apps/gui/files.elf: user/apps/gui/files.o $(DIALOG) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/gui/files.o $(DIALOG) $(GUILIB) $(ULIB) $(LIBGCC)

# the compositor pastes clipboard text into the focused window (Ctrl+V / Super+V),
# so it links the clipboard client lib; it also owns the framebuffer, so Print
# Screen capture (PNG encode) lives here — $(PNGENC) + the shared $(CRC32).
user/apps/gui/compositor.elf: user/apps/gui/compositor.o $(CLIPBOARD) $(PNGENC) $(CRC32) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/gui/compositor.o $(CLIPBOARD) $(PNGENC) $(CRC32) $(GUILIB) $(ULIB) $(LIBGCC)

# The browser is a multi-file user program (user/apps/gui/browser/*.c), linked
# flat: streaming inflate (gzip/deflate), the PNG decoder, HTTP/2, the SQLite jar.
BROWSER_OBJS := user/apps/gui/browser/main.o user/apps/gui/browser/sync.o user/apps/gui/browser/buffers.o user/apps/gui/browser/surface.o user/apps/gui/browser/fetch.o user/apps/gui/browser/resource.o user/apps/gui/browser/html.o user/apps/gui/browser/dom.o user/apps/gui/browser/css/engine.o user/apps/gui/browser/css/sibling.o user/apps/gui/browser/color.o user/apps/gui/browser/layout.o user/apps/gui/browser/anim.o user/apps/gui/browser/svg.o user/apps/gui/browser/jar.o user/apps/gui/browser/cache.o user/apps/gui/browser/js/bindings.o user/apps/gui/browser/js/dom_query.o user/apps/gui/browser/js/indexeddb.o user/apps/gui/browser/js/serviceworker.o user/apps/gui/browser/js/webapi.o user/apps/gui/browser/js/notification.o user/apps/gui/browser/js/media.o user/apps/gui/browser/js/classlist.o user/apps/gui/browser/devtools.o user/apps/gui/browser/csp.o user/apps/gui/browser/cors.o user/apps/gui/browser/imgdec.o user/apps/gui/browser/media.o user/apps/gui/browser/bookmarks.o user/apps/gui/browser/clienthints.o user/apps/gui/browser/permissions.o
user/apps/gui/browser.elf: $(BROWSER_OBJS) $(UPNG) $(WEBP) $(INFLATE) $(WOFF) $(BROTLI) $(URL) $(H2) $(H3) $(QUIC) $(HEADERS) $(HTTPCACHE) $(NETCONN) $(TLS) $(CRYPTO) $(WEBSOCKET) $(SQLITE) $(JS) $(GC) $(MATH) $(MEDIA) $(VIDEO) $(CAST) $(PROTOBUF) $(JSON) $(BASE64) $(CLIPBOARD) $(RESOLVE) $(NOTIFY) $(DIALOG) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ $(BROWSER_OBJS) $(UPNG) $(WEBP) $(INFLATE) $(WOFF) $(BROTLI) $(URL) $(H2) $(H3) $(QUIC) $(HEADERS) $(HTTPCACHE) $(NETCONN) $(TLS) $(CRYPTO) $(WEBSOCKET) $(SQLITE) $(JS) $(GC) $(MATH) $(MEDIA) $(VIDEO) $(CAST) $(PROTOBUF) $(JSON) $(BASE64) $(CLIPBOARD) $(RESOLVE) $(NOTIFY) $(DIALOG) $(GUILIB) $(ULIB) $(LIBGCC)

# notifyd (the notification daemon) decodes notification thumbnails with the
# streaming PNG decoder; it serves the "notify" port and draws the Notification
# Center in its own window.
user/apps/gui/notifyd.elf: user/apps/gui/notifyd.o $(UPNG) $(INFLATE) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/gui/notifyd.o $(UPNG) $(INFLATE) $(GUILIB) $(ULIB) $(LIBGCC)

# the calculator links the math lib for dtoa/strtod (double <-> shortest-decimal).
user/apps/gui/calc.elf: user/apps/gui/calc.o $(MATH) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/gui/calc.o $(MATH) $(GUILIB) $(ULIB) $(LIBGCC)

# the image viewer links the streaming PNG decoder + the WebP codec; player links
# the media engine (decoder + mixer, which bundles the MPEG decoder).
user/apps/gui/imgview.elf: user/apps/gui/imgview.o $(UPNG) $(WEBP) $(INFLATE) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/gui/imgview.o $(UPNG) $(WEBP) $(INFLATE) $(GUILIB) $(ULIB) $(LIBGCC)
# player links the media engine (audio decoder + mixer) plus $(VIDEO) — the H.264
# picture path (which bundles the H.264 decoder + MP4 demuxer) — so it plays both
# audio files and video. $(MATH) is for the AAC decoder's dequant/window trig
# (the MP4 demuxer for .m4a audio comes from $(VIDEO)).
user/apps/gui/player.elf: user/apps/gui/player.o $(MEDIA) $(VIDEO) $(MATH) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/gui/player.o $(MEDIA) $(VIDEO) $(MATH) $(GUILIB) $(ULIB) $(LIBGCC)

# --- CLI apps that pull in extra libs ---------------------------------------
# ping / dig resolve their target in user space now (kernel connect takes an IP
# only): link the resolver ($(RESOLVE) bundles the mDNS client for .local names).
user/apps/cli/ping.elf: user/apps/cli/ping.o $(RESOLVE) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/ping.o $(RESOLVE) $(ULIB) $(LIBGCC)
user/apps/cli/dig.elf: user/apps/cli/dig.o $(RESOLVE) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/dig.o $(RESOLVE) $(ULIB) $(LIBGCC)

# stun prints the host's server-reflexive address (the WebRTC/ICE candidate-
# gathering path); links the STUN codec, which pulls $(CRYPTO) + $(RESOLVE).
user/apps/cli/stun.elf: user/apps/cli/stun.o $(STUN) $(CRC32) $(CRYPTO) $(RESOLVE) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/stun.o $(STUN) $(CRC32) $(CRYPTO) $(RESOLVE) $(ULIB) $(LIBGCC)

# ntpd resolves its server host in user space (kernel connect takes an IP only).
user/apps/cli/ntpd.elf: user/apps/cli/ntpd.o $(RESOLVE) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/ntpd.o $(RESOLVE) $(ULIB) $(LIBGCC)

# clipd (clipboard server) links the clipboard client lib + the MIME helper.
user/apps/cli/clipd.elf: user/apps/cli/clipd.o $(CLIPBOARD) $(MIME) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/clipd.o $(CLIPBOARD) $(MIME) $(ULIB) $(LIBGCC)

# vmclipd bridges the VMware host clipboard (/proc/hostclip) to clipd; links the
# clipboard client lib.
user/apps/cli/vmclipd.elf: user/apps/cli/vmclipd.o $(CLIPBOARD) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/vmclipd.o $(CLIPBOARD) $(ULIB) $(LIBGCC)

# notify-send: the client CLI links the notifyd client lib.
user/apps/cli/notifysend.elf: user/apps/cli/notifysend.o $(NOTIFY) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/notifysend.o $(NOTIFY) $(ULIB) $(LIBGCC)

# webp: the WebP codec CLI (info / encode a PNG to WebP / verify a lossless round
# trip). Links the WebP codec + the streaming PNG decoder it converts from.
user/apps/cli/webp.elf: user/apps/cli/webp.o $(WEBP) $(UPNG) $(INFLATE) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/webp.o $(WEBP) $(UPNG) $(INFLATE) $(ULIB) $(LIBGCC)

# json: the JSON CLI (validate / pretty-print / minify). Links the JSON value-tree
# lib + the math lib it needs for dtoa/strtod.
user/apps/cli/json.elf: user/apps/cli/json.o $(JSON) $(MATH) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/json.o $(JSON) $(MATH) $(ULIB) $(LIBGCC)

# base64: the base64 CLI (encode / -d decode). Links the base64 codec lib.
user/apps/cli/base64.elf: user/apps/cli/base64.o $(BASE64) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/base64.o $(BASE64) $(ULIB) $(LIBGCC)

# mpeg: the MPEG-1 video CLI (info / gen). Links the from-scratch MPEG-1 codec.
user/apps/cli/mpeg.elf: user/apps/cli/mpeg.o $(MPEG) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/mpeg.o $(MPEG) $(ULIB) $(LIBGCC)

# h264: the H.264/AVC video CLI (info). Links the from-scratch baseline decoder
# and the MP4 demuxer (for .mp4/.mov input); $(LIBGCC) for the 64-bit box sizes.
user/apps/cli/h264.elf: user/apps/cli/h264.o $(H264) $(MP4) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/h264.o $(H264) $(MP4) $(ULIB) $(LIBGCC)

# cast: the Cast (Chromecast) sender CLI. Links the cast lib + its transport
# (netconn/TLS/crypto), the CastMessage frame codec ($(PROTOBUF)), the JSON
# payload tree ($(JSON)+$(MATH)) and the resolver ($(RESOLVE), which bundles the
# mDNS client discovery uses).
user/apps/cli/cast.elf: user/apps/cli/cast.o $(CAST) $(PROTOBUF) $(JSON) $(MATH) $(NETCONN) $(TLS) $(CRYPTO) $(RESOLVE) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/cast.o $(CAST) $(PROTOBUF) $(JSON) $(MATH) $(NETCONN) $(TLS) $(CRYPTO) $(RESOLVE) $(ULIB) $(LIBGCC)

# alac: the Apple Lossless codec CLI (info / encode WAV->CAF / decode CAF->WAV /
# verify a bit-exact round trip). Links the ALAC codec; the WAV/CAF containers
# live in the CLI itself.
user/apps/cli/alac.elf: user/apps/cli/alac.o $(ALAC) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/alac.o $(ALAC) $(ULIB) $(LIBGCC)

# aac: the AAC-LC codec CLI (info / decode to WAV). Links the from-scratch AAC
# decoder + the MP4 demuxer (for .m4a input) + $(MATH) (dequant/window trig).
user/apps/cli/aac.elf: user/apps/cli/aac.o $(AAC) $(MP4) $(MATH) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/aac.o $(AAC) $(MP4) $(MATH) $(ULIB) $(LIBGCC)

# curl: libcurl (HTTP/1.1 + HTTP/2 + HTTP/3 over QUIC) + URL + crypto (AES-GCM/x25519).
user/apps/cli/curl.elf: user/apps/cli/curl.o $(HTTP) $(HEADERS) $(H2) $(H3) $(QUIC) $(URL) $(NETCONN) $(TLS) $(CRYPTO) $(BROTLI) $(RESOLVE) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/curl.o $(HTTP) $(HEADERS) $(H2) $(H3) $(QUIC) $(URL) $(NETCONN) $(TLS) $(CRYPTO) $(BROTLI) $(RESOLVE) $(ULIB) $(LIBGCC)

# torrent: libtorrent (bencode/torrent/tracker/peer) + libcurl + crypto (SHA-1).
user/apps/cli/torrent.elf: user/apps/cli/torrent.o $(TORLIB) $(CRYPTO) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/torrent.o $(TORLIB) $(CRYPTO) $(ULIB) $(LIBGCC)

# ssh: from-scratch SSH-2 over net_* sockets + crypto (x25519/SHA-256/AES-128-GCM).
# Resolves its target host in user space ($(RESOLVE)).
user/apps/cli/ssh.elf: user/apps/cli/ssh.o $(CRYPTO) $(RESOLVE) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/ssh.o $(CRYPTO) $(RESOLVE) $(ULIB) $(LIBGCC)

# sshd: the SSH-2 server (ed25519 host-key sign + the x25519/GCM transport).
user/apps/cli/sshd.elf: user/apps/cli/sshd.o $(CRYPTO) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/sshd.o $(CRYPTO) $(ULIB) $(LIBGCC)

# Embed each program ELF as an object. The path-derived symbol is renamed to the
# stable _binary_app_<name>_{start,end} the kernel installer references. Two
# patterns (gui + cli); make picks whichever source actually exists.
kernel/%_blob.o: user/apps/gui/%.elf
	$(OBJCOPY) -I binary -O $(KELF) -B $(KBFD) \
	  --redefine-sym _binary_user_apps_gui_$*_elf_start=_binary_app_$*_start \
	  --redefine-sym _binary_user_apps_gui_$*_elf_end=_binary_app_$*_end $< $@
kernel/%_blob.o: user/apps/cli/%.elf
	$(OBJCOPY) -I binary -O $(KELF) -B $(KBFD) \
	  --redefine-sym _binary_user_apps_cli_$*_elf_start=_binary_app_$*_start \
	  --redefine-sym _binary_user_apps_cli_$*_elf_end=_binary_app_$*_end $< $@

# Combined function-symbol table (one text blob, sections keyed by app name) so
# the crash reporter can name backtrace frames. Format: ">app\n<hexaddr> name\n"…
# per app, sorted by address. Regenerated whenever any listed app's .elf changes.
SYM_APPS := browser files editor term calc imgview player settings notifyd about clock cliphist
kernel/appsyms.o: $(addprefix user/apps/gui/,$(addsuffix .elf,$(SYM_APPS)))
	@: > kernel/appsyms.dat
	@for e in $(filter %.elf,$^); do n=`basename $$e .elf`; printf '>%s\n' "$$n" >> kernel/appsyms.dat; \
	  $(UCROSS)nm -n --defined-only $$e | awk '$$2 ~ /^[tT]$$/ {print $$1" "$$3}' >> kernel/appsyms.dat; done
	$(OBJCOPY) -I binary -O $(KELF) -B $(KBFD) \
	  --redefine-sym _binary_kernel_appsyms_dat_start=_binary_appsyms_start \
	  --redefine-sym _binary_kernel_appsyms_dat_end=_binary_appsyms_end \
	  kernel/appsyms.dat $@
	@rm -f kernel/appsyms.dat

# The shared UI fonts shipped on disk (installed to /etc/fonts at boot); gfx loads
# them at runtime instead of every app baking in a bitmap atlas. Give
# _binary_fonts_Liberation{Sans,Mono}_Regular_ttf_{start,end}.
kernel/uifont_blob.o: fonts/LiberationSans-Regular.ttf
	$(OBJCOPY) -I binary -O $(KELF) -B $(KBFD) $< $@
kernel/monofont_blob.o: fonts/LiberationMono-Regular.ttf
	$(OBJCOPY) -I binary -O $(KELF) -B $(KBFD) $< $@
