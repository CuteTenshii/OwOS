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

# the editor + cliphist use the clipboard (Cut/Copy/Paste) + the MIME text check.
user/apps/gui/editor.elf: user/apps/gui/editor.o $(CLIPBOARD) $(MIME) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/gui/editor.o $(CLIPBOARD) $(MIME) $(GUILIB) $(ULIB) $(LIBGCC)
user/apps/gui/cliphist.elf: user/apps/gui/cliphist.o $(CLIPBOARD) $(MIME) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/gui/cliphist.o $(CLIPBOARD) $(MIME) $(GUILIB) $(ULIB) $(LIBGCC)

# The browser is a multi-file user program (user/apps/gui/browser/*.c), linked
# flat: streaming inflate (gzip/deflate), the PNG decoder, HTTP/2, the SQLite jar.
BROWSER_OBJS := user/apps/gui/browser/main.o user/apps/gui/browser/fetch.o user/apps/gui/browser/resource.o user/apps/gui/browser/html.o user/apps/gui/browser/dom.o user/apps/gui/browser/css.o user/apps/gui/browser/color.o user/apps/gui/browser/layout.o user/apps/gui/browser/svg.o user/apps/gui/browser/jar.o user/apps/gui/browser/js.o user/apps/gui/browser/devtools.o user/apps/gui/browser/csp.o user/apps/gui/browser/cors.o user/apps/gui/browser/media.o
user/apps/gui/browser.elf: $(BROWSER_OBJS) $(UPNG) $(WEBP) $(INFLATE) $(WOFF) $(BROTLI) $(URL) $(H2) $(H3) $(QUIC) $(HEADERS) $(CRYPTO) $(WEBSOCKET) $(SQLITE) $(JS) $(GC) $(MATH) $(MEDIA) $(CLIPBOARD) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ $(BROWSER_OBJS) $(UPNG) $(WEBP) $(INFLATE) $(WOFF) $(BROTLI) $(URL) $(H2) $(H3) $(QUIC) $(HEADERS) $(CRYPTO) $(WEBSOCKET) $(SQLITE) $(JS) $(GC) $(MATH) $(MEDIA) $(CLIPBOARD) $(GUILIB) $(ULIB) $(LIBGCC)

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
user/apps/gui/player.elf: user/apps/gui/player.o $(MEDIA) $(ULIB) $(GUILIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/gui/player.o $(MEDIA) $(GUILIB) $(ULIB) $(LIBGCC)

# --- CLI apps that pull in extra libs ---------------------------------------
# clipd (clipboard server) links the clipboard client lib + the MIME helper.
user/apps/cli/clipd.elf: user/apps/cli/clipd.o $(CLIPBOARD) $(MIME) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/clipd.o $(CLIPBOARD) $(MIME) $(ULIB) $(LIBGCC)

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

# curl: libcurl (HTTP/1.1 + HTTP/2 + HTTP/3 over QUIC) + URL + crypto (AES-GCM/x25519).
user/apps/cli/curl.elf: user/apps/cli/curl.o $(HTTP) $(HEADERS) $(H2) $(H3) $(QUIC) $(URL) $(CRYPTO) $(BROTLI) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/curl.o $(HTTP) $(HEADERS) $(H2) $(H3) $(QUIC) $(URL) $(CRYPTO) $(BROTLI) $(ULIB) $(LIBGCC)

# torrent: libtorrent (bencode/torrent/tracker/peer) + libcurl + crypto (SHA-1).
user/apps/cli/torrent.elf: user/apps/cli/torrent.o $(TORLIB) $(CRYPTO) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/torrent.o $(TORLIB) $(CRYPTO) $(ULIB) $(LIBGCC)

# ssh: from-scratch SSH-2 over net_* sockets + crypto (x25519/SHA-256/AES-128-GCM).
user/apps/cli/ssh.elf: user/apps/cli/ssh.o $(CRYPTO) $(ULIB) user/user.ld
	$(ULD) -m elf_i386 -T user/user.ld -o $@ user/apps/cli/ssh.o $(CRYPTO) $(ULIB) $(LIBGCC)

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

# The shared UI fonts shipped on disk (installed to /etc/fonts at boot); gfx loads
# them at runtime instead of every app baking in a bitmap atlas. Give
# _binary_fonts_Liberation{Sans,Mono}_Regular_ttf_{start,end}.
kernel/uifont_blob.o: fonts/LiberationSans-Regular.ttf
	$(OBJCOPY) -I binary -O $(KELF) -B $(KBFD) $< $@
kernel/monofont_blob.o: fonts/LiberationMono-Regular.ttf
	$(OBJCOPY) -I binary -O $(KELF) -B $(KBFD) $< $@
