# user/apps/build.mk: the user programs, one directory each (user/apps/<name>/),
# and the payload of files the kernel installs onto the disk at boot.
#
# Every app links the base ULIB; GUI (windowed) apps also link gfx + the font lib
# ($(GUILIB)). Per-app extras go in APP_LIBS_<name>. An app is every .c under its
# directory, linked flat, and lands in /bin/<name> (or /bin/$(INSTALL_NAME_<name>)).

# The browser includes its own headers by path from its root.
user/apps/browser/%.o: UCFLAGS += -Iuser/apps/browser

GUI_APPS := about archive browser calc cliphist clock compositor devhub editor files imgview installowos notifyd pdfview player settings term
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
APP_LIBS_file := $(MAGIC)
APP_LIBS_files := $(DIALOG) $(FILEPICK) $(WOFF)
APP_LIBS_h264 := $(H264) $(MP4)
APP_LIBS_imgview := $(PNGDEC) $(IMGANIM)
APP_LIBS_installowos := $(DIALOG)
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

# $(call app_rule,name): link rule for one app.
define app_rule
user/apps/$(1)/$(1).elf: $(call app_objs,$(1)) $(APP_LIBS_$(1)) $(if $(filter $(1),$(GUI_APPS)),$$(GUILIB)) $$(ULIB) user/user.ld
	$$(ULD) -m elf_i386 -T user/user.ld -o $$@ $$(filter %.o,$$^) $$(LIBGCC)
endef
$(foreach a,$(APPS),$(eval $(call app_rule,$(a))))

INSTALL_NAME_notifysend := notify-send

# Start menu entries, installed to /usr/share/applications.
DESKTOP_APPS := term files archive editor browser pdfview player clock calc notifyd settings devhub installowos about

# Combined function-symbol table (one text blob, sections keyed by app name) so
# the crash reporter can name backtrace frames. Format: ">app\n<hexaddr> name\n"...
# per app, sorted by address. Regenerated whenever any listed app's .elf changes.
SYM_APPS := browser files archive editor term calc imgview pdfview player settings devhub notifyd about clock cliphist installowos
kernel/appsyms.o: $(foreach a,$(SYM_APPS),user/apps/$(a)/$(a).elf)
	@: > kernel/appsyms.dat
	@for e in $(filter %.elf,$^); do n=`basename $$e .elf`; printf '>%s\n' "$$n" >> kernel/appsyms.dat; \
	  $(UCROSS)nm -n --defined-only $$e | awk '$$2 ~ /^[tT]$$/ {print $$1" "$$3}' >> kernel/appsyms.dat; done
	$(call blob,kernel/appsyms.dat,appsyms)
	@rm -f kernel/appsyms.dat

# Every fonts/*.ttf and *.otf is installed to /etc/fonts; gfx loads them at runtime
# instead of every app baking in a bitmap atlas. Drop one in fonts/ to ship it.
FONT_FILES := $(sort $(wildcard fonts/*.ttf fonts/*.otf))

# App icons (icons/<Icon= name>.png, from tools/genicons.py), installed to /usr/share/icons.
ICON_NAMES := utilities-terminal system-file-manager utilities-file-archiver accessories-text-editor web-browser multimedia-player \
              preferences-system-time accessories-calculator preferences-system-notifications \
              preferences-system help-about computer application-default \
              audio-volume-muted audio-volume-low audio-volume-high \
              battery-low battery-medium battery-full battery-charging \
              network-ethernet network-ethernet-disconnected

# GRUB images the Installer writes to a disk, built when the host has GRUB for every platform. They carry
# every module they need, since the disk gets no module directory, and find their ESP by a marker file
# the live ISO lacks.
GRUB_PLATFORMS := i386-pc i386-efi x86_64-efi
HAVE_GRUB := $(if $(shell command -v $(GRUB_MKIMAGE) 2>/dev/null),$(if $(strip $(foreach p,$(GRUB_PLATFORMS),$(if $(wildcard $(GRUB_DIR)/$(p)/kernel.img),,missing))),,y))
GRUB_MODULES := part_gpt fat search search_fs_file configfile normal multiboot2 all_video
GRUB_IMAGES := build/grub/core.img build/grub/bootia32.efi build/grub/bootx64.efi
build/grub/early.cfg:
	@mkdir -p $(@D)
	printf 'search --no-floppy --set=root --file /boot/grub/owos.id\nset prefix=($$root)/boot/grub\n' > $@
build/grub/core.img: build/grub/early.cfg
	$(GRUB_MKIMAGE) -d $(GRUB_DIR)/i386-pc -O i386-pc -c $< -p /boot/grub -o $@ biosdisk $(GRUB_MODULES)
build/grub/bootia32.efi: build/grub/early.cfg
	$(GRUB_MKIMAGE) -d $(GRUB_DIR)/i386-efi -O i386-efi -c $< -p /boot/grub -o $@ efi_gop $(GRUB_MODULES)
build/grub/bootx64.efi: build/grub/early.cfg
	$(GRUB_MKIMAGE) -d $(GRUB_DIR)/x86_64-efi -O x86_64-efi -c $< -p /boot/grub -o $@ efi_gop $(GRUB_MODULES)

# Everything the kernel embeds and installs at boot (kernel/install.c), as mode:disk-path:source.
PAYLOAD := $(foreach a,$(APPS),0755:/bin/$(or $(INSTALL_NAME_$(a)),$(a)):user/apps/$(a)/$(a).elf) \
           $(foreach a,$(DESKTOP_APPS),0644:/usr/share/applications/$(a).desktop:user/apps/$(a)/$(a).desktop) \
           $(foreach i,$(ICON_NAMES),0644:/usr/share/icons/$(i).png:icons/$(i).png) \
           $(foreach f,$(FONT_FILES),0644:/etc/fonts/$(notdir $(f)):$(f)) \
           $(if $(HAVE_GRUB),0644:/usr/lib/grub/boot.img:$(GRUB_DIR)/i386-pc/boot.img \
             $(foreach i,$(GRUB_IMAGES),0644:/usr/lib/grub/$(notdir $(i)):$(i)))
PAYLOAD_SRCS := $(foreach e,$(PAYLOAD),$(word 3,$(subst :, ,$(e))))

# The list is rewritten only when it changes, so dropping an entry still regenerates payload.c.
kernel/payload.list: FORCE
	@printf '%s %s %s\n' $(subst :, ,$(PAYLOAD)) > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi
kernel/payload.c: kernel/payload.list $(PAYLOAD_SRCS) tools/genpayload.sh
	tools/genpayload.sh $< > $@
