# user/apps/build.mk: the user programs, one directory each (user/apps/<name>/),
# embedded into the kernel as blobs.
#
# Every app links the base ULIB; GUI (windowed) apps also link gfx + the font lib
# ($(GUILIB)). Per-app extras go in APP_LIBS_<name>; an app with several sources
# lists them in APP_OBJS_<name> (default: user/apps/<name>/<name>.o). Each ELF is
# wrapped into kernel/<name>_blob.o with the symbol normalised to
# _binary_app_<name>_{start,end}.

# The browser includes its own headers by path from its root.
user/apps/browser/%.o: UCFLAGS += -Iuser/apps/browser

GUI_APPS := about browser calc cliphist clock compositor editor files imgview notifyd player settings term
CLI_APPS := aac alac base64 cast chmod chown clipd curl dhcpcd dig edit h264 init json lspci mdnsd mediactl mediad mpeg neofetch notifysend ntpd ping sh smbd ssh sshd stun torrent uname unrar vmclipd webp
APPS := $(GUI_APPS) $(CLI_APPS)

# --- per-app libraries ---------------------------------------------------
# compositor: clipboard paste, plus Print Screen capture (PNG encode + CRC-32).
# browser: a multi-file program, linked flat.
APP_OBJS_compositor := user/apps/compositor/compositor.o user/apps/compositor/compositor_damage.o
APP_OBJS_browser := user/apps/browser/main.o user/apps/browser/tabs.o user/apps/browser/worker.o user/apps/browser/text.o user/apps/browser/flow.o user/apps/browser/image.o user/apps/browser/paint.o user/apps/browser/chrome.o user/apps/browser/render.o user/apps/browser/input.o user/apps/browser/sync.o user/apps/browser/buffers.o user/apps/browser/surface.o user/apps/browser/fetch.o user/apps/browser/resource.o user/apps/browser/html.o user/apps/browser/html_entities.o user/apps/browser/html_text.o user/apps/browser/html_embed.o user/apps/browser/html_forms.o user/apps/browser/html_walk.o user/apps/browser/html_res.o user/apps/browser/dom.o user/apps/browser/css/engine.o user/apps/browser/css/engine_values.o user/apps/browser/css/engine_props.o user/apps/browser/css/engine_decl.o user/apps/browser/css/engine_sel.o user/apps/browser/css/engine_sheet.o user/apps/browser/css/sibling.o user/apps/browser/color.o user/apps/browser/layout.o user/apps/browser/features/anim/anim.o user/apps/browser/features/svg/svg.o user/apps/browser/features/jar/jar.o user/apps/browser/features/cache/cache.o user/apps/browser/js/bindings.o user/apps/browser/js/dom_query.o user/apps/browser/features/indexeddb/indexeddb.o user/apps/browser/features/serviceworker/serviceworker.o user/apps/browser/js/webapi.o user/apps/browser/features/notification/notification.o user/apps/browser/js/media.o user/apps/browser/js/classlist.o user/apps/browser/features/devtools/devtools.o user/apps/browser/features/devtools/devtools_log.o user/apps/browser/features/devtools/devtools_snap.o user/apps/browser/features/devtools/devtools_render.o user/apps/browser/features/csp/csp.o user/apps/browser/features/cors/cors.o user/apps/browser/imgdec.o user/apps/browser/features/media/media.o user/apps/browser/features/bookmarks/bookmarks.o user/apps/browser/features/clienthints/clienthints.o user/apps/browser/features/permissions/permissions.o user/apps/browser/features/mediasession/mediasession.o
APP_LIBS_aac := $(AAC) $(MP4) $(MATH)
APP_LIBS_alac := $(ALAC)
APP_LIBS_base64 := $(BASE64)
APP_LIBS_browser := $(UPNG) $(WEBP) $(INFLATE) $(WOFF) $(BROTLI) $(URL) $(H2) $(H3) $(QUIC) $(HEADERS) $(HTTPCACHE) $(NETCONN) $(TLS) $(CRYPTO) $(WEBSOCKET) $(SQLITE) $(JS) $(GC) $(MATH) $(MEDIA) $(VIDEO) $(CAST) $(PROTOBUF) $(JSON) $(BASE64) $(CLIPBOARD) $(RESOLVE) $(NOTIFY) $(DIALOG) $(MEDIACTL)
APP_LIBS_calc := $(MATH)
APP_LIBS_cast := $(CAST) $(PROTOBUF) $(JSON) $(MATH) $(NETCONN) $(TLS) $(CRYPTO) $(RESOLVE)
APP_LIBS_clipd := $(CLIPBOARD) $(MIME)
APP_LIBS_cliphist := $(CLIPBOARD) $(MIME)
APP_LIBS_compositor := $(CLIPBOARD) $(PNGENC) $(CRC32) $(MEDIACTL) $(DESKTOP)
APP_LIBS_curl := $(HTTP) $(HEADERS) $(H2) $(H3) $(QUIC) $(URL) $(NETCONN) $(TLS) $(CRYPTO) $(BROTLI) $(RESOLVE)
APP_LIBS_dig := $(RESOLVE)
APP_LIBS_editor := $(CLIPBOARD) $(MIME) $(DIALOG)
APP_LIBS_files := $(DIALOG)
APP_LIBS_h264 := $(H264) $(MP4)
APP_LIBS_imgview := $(UPNG) $(WEBP) $(INFLATE)
APP_LIBS_json := $(JSON) $(MATH)
APP_LIBS_mediactl := $(MEDIACTL)
APP_LIBS_mediad := user/lib/mediactl/session.o
APP_LIBS_mpeg := $(MPEG)
APP_LIBS_notifyd := $(UPNG) $(INFLATE)
APP_LIBS_notifysend := $(NOTIFY)
APP_LIBS_ntpd := $(RESOLVE) $(NTP)
APP_LIBS_ping := $(RESOLVE)
APP_LIBS_player := $(MEDIA) $(VIDEO) $(MATH) $(MEDIACTL)
APP_LIBS_ssh := $(CRYPTO) $(RESOLVE)
APP_LIBS_sshd := $(CRYPTO)
APP_LIBS_stun := $(STUN) $(CRC32) $(CRYPTO) $(RESOLVE)
APP_LIBS_unrar := $(RAR) $(CRYPTO) $(CRC32)
APP_LIBS_term :=
APP_LIBS_sh := $(TERMLIB)
APP_LIBS_torrent := $(TORLIB) $(CRYPTO)
APP_LIBS_vmclipd := $(CLIPBOARD)
APP_LIBS_webp := $(WEBP) $(UPNG) $(INFLATE)

# $(call app_rule,name): link rule + kernel blob rule for one app.
define app_rule
user/apps/$(1)/$(1).elf: $(or $(APP_OBJS_$(1)),user/apps/$(1)/$(1).o) $(APP_LIBS_$(1)) $(if $(filter $(1),$(GUI_APPS)),$$(GUILIB)) $$(ULIB) user/user.ld
	$$(ULD) -m elf_i386 -T user/user.ld -o $$@ $$(filter %.o,$$^) $$(LIBGCC)
kernel/$(1)_blob.o: user/apps/$(1)/$(1).elf
	$$(OBJCOPY) -I binary -O $$(KELF) -B $$(KBFD) \
	  --redefine-sym _binary_user_apps_$(1)_$(1)_elf_start=_binary_app_$(1)_start \
	  --redefine-sym _binary_user_apps_$(1)_$(1)_elf_end=_binary_app_$(1)_end $$< $$@
endef
$(foreach a,$(APPS),$(eval $(call app_rule,$(a))))

# Start menu entries, embedded in the kernel and installed to /usr/share/applications at boot.
DESKTOP_APPS := term files editor browser player clock calc notifyd settings about
define desktop_rule
kernel/$(1)_desktop_blob.o: user/apps/$(1)/$(1).desktop
	$$(OBJCOPY) -I binary -O $$(KELF) -B $$(KBFD) \
	  --redefine-sym _binary_user_apps_$(1)_$(1)_desktop_start=_binary_desktop_$(1)_start \
	  --redefine-sym _binary_user_apps_$(1)_$(1)_desktop_end=_binary_desktop_$(1)_end $$< $$@
endef
$(foreach a,$(DESKTOP_APPS),$(eval $(call desktop_rule,$(a))))

# Combined function-symbol table (one text blob, sections keyed by app name) so
# the crash reporter can name backtrace frames. Format: ">app\n<hexaddr> name\n"...
# per app, sorted by address. Regenerated whenever any listed app's .elf changes.
SYM_APPS := browser files editor term calc imgview player settings notifyd about clock cliphist
kernel/appsyms.o: $(foreach a,$(SYM_APPS),user/apps/$(a)/$(a).elf) $(ARCH_STAMP)
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
kernel/uifont_blob.o: fonts/LiberationSans-Regular.ttf $(ARCH_STAMP)
	$(OBJCOPY) -I binary -O $(KELF) -B $(KBFD) $< $@
kernel/monofont_blob.o: fonts/LiberationMono-Regular.ttf $(ARCH_STAMP)
	$(OBJCOPY) -I binary -O $(KELF) -B $(KBFD) $< $@
