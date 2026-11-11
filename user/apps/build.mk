# user/apps/build.mk: the user programs, one directory each (user/apps/<name>/),
# embedded into the kernel as blobs.
#
# Every app links the base ULIB; GUI (windowed) apps also link gfx + the font lib
# ($(GUILIB)). Per-app extras go in APP_LIBS_<name>. An app is every .c under its
# directory, linked flat. Each ELF is wrapped into kernel/<name>_blob.o with the
# symbol normalised to _binary_app_<name>_{start,end}.

# The browser includes its own headers by path from its root.
user/apps/browser/%.o: UCFLAGS += -Iuser/apps/browser

GUI_APPS := about archive browser calc cliphist clock compositor devhub editor files imgview notifyd pdfview player settings term
APPS := $(patsubst user/apps/%/,%,$(wildcard user/apps/*/))
app_objs = $(patsubst %.c,%.o,$(sort $(shell find user/apps/$(1) -name '*.c')))

# --- per-app libraries ---------------------------------------------------
APP_LIBS_aac := $(AAC) $(MP4)
APP_LIBS_alac := $(ALAC)
APP_LIBS_archive := $(ARCHIVE) $(DIALOG) $(FILEPICK)
APP_LIBS_base64 := $(BASE64)
APP_LIBS_browser := $(JPEG) $(PNGDEC) $(IMGANIM) $(CANVAS2D) $(DEFLATE) $(WOFF) $(URL) $(H2) $(H3) $(HTTPCACHE) $(WEBSOCKET) $(MIME) $(SQLITE) $(JS) $(MEDIA) $(VIDEO) $(CAST) $(AIRPLAY) $(CLIPBOARD) $(NOTIFY) $(DIALOG) $(FILEPICK) $(MEDIACTL)
APP_LIBS_calc := $(MATH)
APP_LIBS_cast := $(CAST)
APP_LIBS_clipd := $(CLIPBOARD) $(MIME)
APP_LIBS_cliphist := $(CLIPBOARD) $(MIME)
# compositor: clipboard paste, plus Print Screen capture (PNG encode + CRC-32).
APP_LIBS_compositor := $(CLIPBOARD) $(PNGENC) $(MEDIACTL) $(DESKTOP) $(PNGDEC)
APP_LIBS_curl := $(HTTP) $(BROTLI)
APP_LIBS_dig := $(RESOLVE)
APP_LIBS_editor := $(CLIPBOARD) $(MIME) $(DIALOG) $(FILEPICK)
APP_LIBS_files := $(DIALOG) $(FILEPICK) $(WOFF)
APP_LIBS_h264 := $(H264) $(MP4)
APP_LIBS_imgview := $(PNGDEC) $(IMGANIM)
APP_LIBS_json := $(JSON)
APP_LIBS_mediactl := $(MEDIACTL)
APP_LIBS_mediad := user/lib/mediactl/session.o
APP_LIBS_mpeg := $(MPEG)
APP_LIBS_notifyd := $(PNGDEC)
APP_LIBS_notifysend := $(NOTIFY)
APP_LIBS_trayd := $(TRAY)
APP_LIBS_ntpd := $(RESOLVE) $(NTP)
APP_LIBS_ping := $(RESOLVE)
APP_LIBS_pdfview := $(PDF) $(FILEPICK) $(DIALOG) $(CLIPBOARD) $(MIME)
APP_LIBS_player := $(FILEPICK) $(MEDIA) $(VIDEO) $(JPEG) $(PNGDEC) $(WEBP) $(MEDIACTL) $(TRAY)
APP_LIBS_ssh := $(CRYPTO) $(RESOLVE)
APP_LIBS_sshd := $(CRYPTO)
APP_LIBS_stun := $(STUN)
APP_LIBS_unrar := $(RAR)
APP_LIBS_term :=
APP_LIBS_sh := $(SHLIB)
APP_LIBS_settings := $(HTTP) $(BROTLI)
APP_LIBS_torrent := $(TORLIB)
APP_LIBS_upnpc := $(UPNP)
APP_LIBS_vmclipd := $(CLIPBOARD)
APP_LIBS_webp := $(WEBP) $(PNGDEC)

# $(call app_rule,name): link rule + kernel blob rule for one app.
define app_rule
user/apps/$(1)/$(1).elf: $(call app_objs,$(1)) $(APP_LIBS_$(1)) $(if $(filter $(1),$(GUI_APPS)),$$(GUILIB)) $$(ULIB) user/user.ld
	$$(ULD) -m elf_i386 -T user/user.ld -o $$@ $$(filter %.o,$$^) $$(LIBGCC)
kernel/$(1)_blob.o: user/apps/$(1)/$(1).elf
	$$(call blob,$$<,app_$(1))
endef
$(foreach a,$(APPS),$(eval $(call app_rule,$(a))))

# Start menu entries, embedded in the kernel and installed to /usr/share/applications at boot.
DESKTOP_APPS := term files archive editor browser pdfview player clock calc notifyd settings devhub about
define desktop_rule
kernel/$(1)_desktop_blob.o: user/apps/$(1)/$(1).desktop
	$$(call blob,$$<,desktop_$(1))
endef
$(foreach a,$(DESKTOP_APPS),$(eval $(call desktop_rule,$(a))))

# Combined function-symbol table (one text blob, sections keyed by app name) so
# the crash reporter can name backtrace frames. Format: ">app\n<hexaddr> name\n"...
# per app, sorted by address. Regenerated whenever any listed app's .elf changes.
SYM_APPS := browser files archive editor term calc imgview pdfview player settings devhub notifyd about clock cliphist
kernel/appsyms.o: $(foreach a,$(SYM_APPS),user/apps/$(a)/$(a).elf)
	@: > kernel/appsyms.dat
	@for e in $(filter %.elf,$^); do n=`basename $$e .elf`; printf '>%s\n' "$$n" >> kernel/appsyms.dat; \
	  $(UCROSS)nm -n --defined-only $$e | awk '$$2 ~ /^[tT]$$/ {print $$1" "$$3}' >> kernel/appsyms.dat; done
	$(call blob,kernel/appsyms.dat,appsyms)
	@rm -f kernel/appsyms.dat

# Every fonts/*.ttf and *.otf is installed to /etc/fonts at boot; gfx loads them at runtime
# instead of every app baking in a bitmap atlas. Drop one in fonts/ to ship it.
FONT_FILES := $(sort $(wildcard fonts/*.ttf fonts/*.otf))
kernel/fontstore.c: $(FONT_FILES) tools/genfontstore.sh
	tools/genfontstore.sh $(FONT_FILES) > $@

# App icons (icons/<Icon= name>.png, from tools/genicons.py), embedded in the kernel and installed to
# /usr/share/icons at boot. The symbol base has dashes turned into underscores.
ICON_NAMES := utilities-terminal system-file-manager utilities-file-archiver accessories-text-editor web-browser multimedia-player \
              preferences-system-time accessories-calculator preferences-system-notifications \
              preferences-system help-about computer application-default \
              audio-volume-muted audio-volume-low audio-volume-high \
              battery-low battery-medium battery-full battery-charging \
              network-ethernet network-ethernet-disconnected
define icon_rule
kernel/icon_$(subst -,_,$(1))_blob.o: icons/$(1).png
	$$(call blob,$$<,icons_$(subst -,_,$(1))_png)
endef
$(foreach i,$(ICON_NAMES),$(eval $(call icon_rule,$(i))))
