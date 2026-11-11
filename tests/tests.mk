# Host test build, driven by tests/run-tests.sh. Objects are cached in build/tests and rebuilt from
# dependency files, so only what changed recompiles; `make -j` compiles in parallel.
#   tests/run-tests.sh [-v] [-l] [-jN] [suite...]

CC      ?= cc
B       := build/tests
RUN     := $(abspath $(B))/run

INCLUDES := -Itests -Iuser/lib $(patsubst %/,-I%,$(wildcard user/lib/*/)) -Iuser/apps/browser -Iuser/apps/browser/js
CFLAGS_T := -std=gnu11 -O2 $(INCLUDES) -MMD -MP

# Host compressors the btrfs test cross-checks against; each is used only when pkg-config finds it.
HOST_PKGS  := $(foreach e,ZLIB:zlib ZSTD:libzstd LZO:lzo2,$(if $(shell pkg-config --exists $(word 2,$(subst :, ,$(e))) 2>/dev/null && echo y),$(e)))
HOST_DEFS  := $(foreach e,$(HOST_PKGS),-DHAVE_HOST_$(word 1,$(subst :, ,$(e))))
HOST_LIBS  := $(foreach e,$(HOST_PKGS),$(shell pkg-config --libs $(word 2,$(subst :, ,$(e)))))
ifneq ($(filter LZO:%,$(HOST_PKGS)),)
HOST_DEFS  += -I$(dir $(shell pkg-config --variable=includedir lzo2))
endif

BROWSER := user/apps/browser
LIB     := user/lib
# Sources shared by both test binaries
COMMON_SRCS := $(BROWSER)/css/engine.c $(BROWSER)/css/engine_values.c $(BROWSER)/css/engine_props.c \
  $(BROWSER)/css/engine_decl.c $(BROWSER)/css/engine_ext.c $(BROWSER)/css/engine_sel.c $(BROWSER)/css/engine_sheet.c \
  $(BROWSER)/css/sibling.c $(BROWSER)/color.c $(BROWSER)/dom.c $(BROWSER)/features/csp/csp.c \
  $(BROWSER)/js/classlist.c $(BROWSER)/js/webapi.c $(BROWSER)/features/notification/notification.c \
  $(LIB)/curl/url.c $(LIB)/curl/httpcache.c $(LIB)/curl/headers.c $(LIB)/crypto/crypto.c \
  $(LIB)/base64/base64.c $(LIB)/sqlite/sqlite.c $(LIB)/js/lexer.c $(LIB)/js/parser.c $(LIB)/js/ast.c \
  $(LIB)/js/value.c $(LIB)/js/interp.c $(LIB)/js/builtins.c $(LIB)/js/bigint.c $(LIB)/js/wasm_js.c \
  $(LIB)/wasm/wasm.c $(LIB)/wasm/wasm_run.c $(LIB)/wasm/wasm_simd.c $(LIB)/math/fmt.c \
  $(LIB)/gc/gc.c tests/host_math.c $(LIB)/opt.c $(LIB)/mutex.c
MAIN_SRCS := $(COMMON_SRCS) $(LIB)/json/json.c $(wildcard $(BROWSER)/features/svg/*.c) $(BROWSER)/imgdec.c $(LIB)/mime/mime.c $(LIB)/gfx/scrollview.c $(LIB)/gfx/textfield.c $(LIB)/gfx/toggle.c $(LIB)/zlib/crc32.c kernel/crc32c.c $(LIB)/stun/stun.c $(LIB)/xml/xml.c $(LIB)/xml/xmltree.c $(LIB)/upnp/upnp.c $(LIB)/ntp/ntp.c $(LIB)/plist/plist.c $(LIB)/uuid/uuid.c \
  $(LIB)/font/font.c $(LIB)/font/woff.c $(LIB)/mediactl/session.c $(LIB)/mp4/mp4.c $(LIB)/media/tags.c $(LIB)/jpeg/jpeg.c $(wildcard $(LIB)/rar/*.c) $(LIB)/brotli/decode.c \
  drivers/usb/hid_desc.c drivers/usb/hid_pad.c drivers/usb/uvc_desc.c drivers/usb/usb3.c drivers/video/dpaux.c drivers/video/edidparse.c drivers/video/blpwm.c $(wildcard drivers/bt/*.c) $(LIB)/png/png.c $(LIB)/zlib/inflate.c $(LIB)/webp/webp.c $(LIB)/webp/vp8.c $(LIB)/gif/gif.c $(LIB)/imganim/imganim.c \
  $(wildcard $(LIB)/canvas2d/*.c) $(wildcard $(LIB)/pdf/*.c) \
  $(LIB)/zstd/zstd_common.c $(LIB)/zstd/fse.c $(LIB)/zstd/huf.c $(LIB)/zstd/zstd_enc.c kernel/arch/common/fdt.c kernel/arch/common/memmap.c \
  user/apps/devhub/parse.c
BIND_SRCS := $(COMMON_SRCS) $(BROWSER)/features/battery/battery.c $(LIB)/uuid/uuid.c $(BROWSER)/html_entities.c $(LIB)/mime/mime.c $(BROWSER)/js/dom_query.c $(BROWSER)/features/cors/cors.c $(BROWSER)/blobstore.c $(BROWSER)/features/canvas/canvas.c \
  $(wildcard $(LIB)/canvas2d/*.c) $(LIB)/font/font.c $(LIB)/zlib/crc32.c $(LIB)/zlib/inflate.c $(LIB)/zlib/deflate.c

# Make 4.3+ passes `\#` inside a function call through with its backslash, older versions need it; HASH works in both.
HASH := \#
MAIN_SUITES := $(sort $(patsubst tests/%_test.c,%,$(shell grep -l '^$(HASH)define OWOS_SUITE_TEST$$' tests/*_test.c)))
BIND_SUITES := $(sort $(patsubst tests/%_test.c,%,$(shell grep -l '^$(HASH)define OWOS_SUITE_BINDINGS$$' tests/*_test.c)))

# Per-suite compile flags
EXTRA_permissions := -DPERM_DB='"$(RUN)/permissions.db"'
EXTRA_bindings_dom := -DSTORAGE_DB='"$(RUN)/bind-localstorage.db"'
EXTRA_tz          := -DTZ_CONFIG_PATH='"$(RUN)/timezone"'
EXTRA_vary        := -DCACHE_DIR='"$(RUN)/vary-cache"'
EXTRA_btrfs       := $(HOST_DEFS)
EXTRA_net6        := -Idrivers/net
# kernel/zstd.c (pulled in by the btrfs suite) also defines zstd_decompress; the user-space codec is renamed here
ZSTD_RENAME       := -Dzstd_decompress=user_zstd_decompress
ARCHIVE_HOST_FS   := -Dmkdir=archive_test_mkdir -Dstream_begin=archive_test_stream_begin \
                     -Dstream_write=archive_test_stream_write -Dstream_end=archive_test_stream_end -include tests/archive_fs.h
EXTRA_zstd        := $(ZSTD_RENAME)
EXTRA_a64_elf     := -Ikernel

# Objects rebuilt with test-specific defines or a fake system layer: impl_<name> := <source> <extra flags>
# builds $(B)/impl/<name>.o, and bind_<name> the same under $(B)/bind/impl for the bindings binary.
impl_netconn       := $(LIB)/netconn/netconn.c -I$(LIB)/tls -Dmsec=ncp_msec -Dnet_dial=ncp_dial -Dnet_close=ncp_close \
  -Dnet_recv_nb=ncp_recv_nb -Dnet_recv=ncp_recv -Dnet_send=ncp_send -Dtls_client=ncp_tls_client \
  -Dtls_last_error=ncp_tls_last_error -Dtls_read=ncp_tls_read -Dtls_write=ncp_tls_write -Dtls_alpn=ncp_tls_alpn \
  -Dtls_verify_message=ncp_tls_verify_message -Dtls_close=ncp_tls_close -Dtls_peer_closed=ncp_tls_peer_closed
impl_h3            := $(LIB)/curl/h3.c -Dquic_connect_early=h3t_connect_early -Dquic_finish_handshake=h3t_finish \
  -Dquic_handshake_done=h3t_hs_done -Dquic_open=h3t_open -Dquic_send=h3t_send -Dquic_read=h3t_read \
  -Dquic_stream_done=h3t_done -Dquic_next_uni=h3t_next_uni -Dquic_poll=h3t_poll -Dquic_peer_closed=h3t_peer_closed \
  -Dquic_streams_left=h3t_left -Dquic_idle_ms=h3t_idle -Dquic_close=h3t_close \
  -Dnc_idle_put=h3t_idle_put -Dnc_idle_take=h3t_idle_take
impl_h2            := $(LIB)/curl/h2.c -Dnc_read=h2t_read -Dnc_write=h2t_write -Dnc_close=h2t_close \
  -Dnc_peer_closed=h2t_peer_closed -Dnc_idle_put=h2t_idle_put -Dnc_idle_take=h2t_idle_take
impl_permissions   := $(BROWSER)/features/permissions/permissions.c -DPERM_DB='"$(RUN)/permissions.db"'
impl_cache         := $(BROWSER)/features/cache/cache.c -DCACHE_DIR='"$(RUN)/vary-cache"' -Dmkdir=vary_mkdir \
  -Dunlink=vary_unlink -Dreaddir=vary_readdir -include tests/vary_fs.h
impl_vary_fs       := tests/vary_fs.c -DCACHE_DIR='"$(RUN)/vary-cache"'
# The UVC driver against kernel service stand-ins (tests/uvc_host); sched_yield is renamed so the test supplies it
impl_uvc           := drivers/usb/uvc.c -Itests/uvc_host -Dsched_yield=uvc_host_yield
impl_tz            := $(LIB)/tz/tz.c -DTZ_CONFIG_PATH='"$(RUN)/timezone"'
impl_desktop       := $(LIB)/desktop/desktop.c -Dreaddir=desktop_test_readdir -include tests/desktop_fs.h
impl_filelist      := $(LIB)/gfx/filelist.c -Dreaddir=filelist_test_readdir -include tests/filelist_fs.h
impl_zstd_dec      := $(LIB)/zstd/zstd_dec.c $(ZSTD_RENAME)
impl_archive       := $(LIB)/archive/archive.c $(ZSTD_RENAME) $(ARCHIVE_HOST_FS)
impl_zip           := $(LIB)/archive/zip.c $(ARCHIVE_HOST_FS)
impl_browser_js_host := tests/browser_js_host.c
impl_test_runner   := tests/test_runner.c
impl_brotli_dict   := $(B)/impl/brotli_dict.c
# Kernel sources behind a host stand-in for their hardware layer (interrupt flags, physical frame access)
impl_pmm           := kernel/pmm.c -include tests/irqflags_host.h
impl_a64_elf       := kernel/arch/aarch64/elf.c -Ikernel -include tests/a64_host.h
impl_fbcon         := kernel/arch/aarch64/fbcon.c -Ikernel
impl_clk           := kernel/arch/aarch64/clk.c -include tests/clk_host.h
impl_gpio          := kernel/arch/aarch64/gpio.c -Ikernel -include tests/gpio_host.h -include tests/spinlock_host.h
impl_pwrap         := kernel/arch/aarch64/pwrap.c -Ikernel -include tests/pwrap_host.h
impl_i2c           := kernel/arch/aarch64/i2c.c -Ikernel -Ikernel/arch/common -include tests/i2c_mtk_host.h
impl_i2c_mtk       := kernel/arch/aarch64/i2c_mtk.c -include tests/i2c_mtk_host.h
impl_tegra_car     := kernel/arch/aarch64/tegra_car.c -include tests/tegra_host.h
impl_tegra_gpio    := kernel/arch/aarch64/tegra_gpio.c -include tests/tegra_host.h
impl_i2c_tegra     := kernel/arch/aarch64/i2c_tegra.c -include tests/tegra_host.h
impl_max77620      := kernel/arch/aarch64/max77620.c -include tests/tegra_host.h

# Suites that link the real browser bindings get their own binary
BIND_HOST := -include tests/bindings_host.h
BINDINGS_PARTS := bindings events navigator cssom runner clipboard blob canvas canvaspath canvasimg xhr realtime timers webworker messagechannel webcomponents dialog forms url fetchapi navigation elementapi documentapi xpath namespace shadow streams streamscore filedata webaudio audioworklet
$(foreach p,$(BINDINGS_PARTS),$(eval bind_$(p) := $(BROWSER)/js/$(p).c $(BIND_HOST) -Drealloc=poison_realloc))
bind_jar           := $(BROWSER)/features/jar/jar.c $(BIND_HOST) -Drealloc=poison_realloc
bind_storage       := $(BROWSER)/features/storage/storage.c $(BIND_HOST) -DSTORAGE_DB='"$(RUN)/bind-localstorage.db"'
bind_indexeddb     := $(BROWSER)/features/indexeddb/indexeddb.c $(BIND_HOST)
bind_serviceworker := $(BROWSER)/features/serviceworker/serviceworker.c $(BIND_HOST)
bind_persist       := $(BROWSER)/features/persist/persist.c $(BIND_HOST) -DPERSIST_PREFIX='"$(RUN)/bind-"'
bind_clienthints   := $(BROWSER)/features/clienthints/clienthints.c $(BIND_HOST) -Drealloc=poison_realloc
bind_permissions   := $(BROWSER)/features/permissions/permissions.c -DPERM_DB='"$(RUN)/bind-permissions.db"'
bind_host          := tests/bindings_host.c

table_names = $(sort $(patsubst $(1)_%,%,$(filter $(1)_%,$(.VARIABLES))))
IMPLS      := $(patsubst %,$(B)/impl/%.o,$(call table_names,impl))
BIND_IMPLS := $(patsubst %,$(B)/bind/impl/%.o,$(call table_names,bind)) $(B)/impl/test_runner.o

MAIN_OBJS := $(MAIN_SUITES:%=$(B)/suite/%.o) $(IMPLS) $(MAIN_SRCS:%.c=$(B)/obj/%.o)
BIND_OBJS := $(BIND_SUITES:%=$(B)/bind/suite/%.o) $(BIND_IMPLS) $(BIND_SRCS:%.c=$(B)/obj/%.o)

# Flags live in this file, so editing it rebuilds every object
$(MAIN_OBJS) $(BIND_OBJS): tests/tests.mk

.PHONY: build
build: $(B)/owos-tests $(B)/owos-bindings-tests

$(B)/owos-tests: $(MAIN_OBJS)
	@$(CC) $^ $(HOST_LIBS) -lm -lpthread -o $@
$(B)/owos-bindings-tests: $(BIND_OBJS)
	@$(CC) $^ -lm -lpthread -o $@

COMPILE = @mkdir -p $(@D) && $(CC) $(CFLAGS_T) $(1) -c $< -o $@
SUITE_FLAGS = -Dmain=owos_main_$* -DOWOS_TEST_NAME=$* -include tests/test.h

$(B)/obj/%.o: %.c
	$(call COMPILE,)
$(B)/suite/%.o: tests/%_test.c
	$(call COMPILE,$(SUITE_FLAGS) $(EXTRA_$*))
$(B)/bind/suite/%.o: tests/%_test.c
	$(call COMPILE,$(SUITE_FLAGS) $(EXTRA_$*))

# The source is the first word of the object's table entry, the extra flags the rest.
.SECONDEXPANSION:
$(IMPLS): $(B)/impl/%.o: $$(firstword $$(impl_$$*))
	$(call COMPILE,$(wordlist 2,$(words $(impl_$*)),$(impl_$*)))
$(filter $(B)/bind/%,$(BIND_IMPLS)): $(B)/bind/impl/%.o: $$(firstword $$(bind_$$*))
	$(call COMPILE,$(wordlist 2,$(words $(bind_$*)),$(bind_$*)))

# The Brotli static dictionary is an objcopy'd blob on the target; embed it as a C array here
$(B)/impl/brotli_dict.c: $(LIB)/brotli/dictionary.bin
	@mkdir -p $(@D) && { echo 'const unsigned char _binary_user_lib_brotli_dictionary_bin_start[] = {'; \
	  od -An -v -tu1 $< | awk '{ for (i = 1; i <= NF; i++) printf "%s,", $$i; print "" }'; echo '};'; } > $@
-include $(shell find $(B) -name '*.d' 2>/dev/null)
