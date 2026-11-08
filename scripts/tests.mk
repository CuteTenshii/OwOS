# Host test build, driven by scripts/run-tests.sh. Objects are cached in build/tests and rebuilt from
# dependency files, so only what changed recompiles; `make -j` compiles in parallel.
#   scripts/run-tests.sh [-v] [-l] [-jN] [suite...]

CC      ?= cc
B       := build/tests
RUN     := $(abspath $(B))/run

INCLUDES := -Iscripts -Itests -Iuser/lib -Iuser/lib/js -Iuser/lib/math -Iuser/lib/gc -Iuser/lib/base64 \
            -Iuser/lib/curl -Iuser/lib/crypto -Iuser/lib/resolve -Iuser/lib/zlib -Iuser/lib/stun \
            -Iuser/lib/mp4 -Iuser/lib/sqlite -Iuser/lib/gfx -Iuser/lib/png -Iuser/lib/webp -Iuser/lib/gif -Iuser/lib/imganim -Iuser/lib/canvas2d -Iuser/lib/font -Iuser/lib/tz -Iuser/lib/desktop \
            -Iuser/lib/ntp -Iuser/lib/netconn -Iuser/lib/notify -Iuser/lib/mediactl -Iuser/apps/browser \
            -Iuser/apps/browser/js -Iuser/lib/cast -Iuser/lib/clipboard -Iuser/lib/websocket \
            -Iuser/lib/brotli -Iuser/lib/rar -Iuser/lib/mime -Iuser/lib/font
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
  $(BROWSER)/css/engine_decl.c $(BROWSER)/css/engine_sel.c $(BROWSER)/css/engine_sheet.c \
  $(BROWSER)/css/sibling.c $(BROWSER)/color.c $(BROWSER)/dom.c $(BROWSER)/features/csp/csp.c \
  $(BROWSER)/js/classlist.c $(BROWSER)/js/webapi.c $(BROWSER)/features/notification/notification.c \
  $(LIB)/curl/url.c $(LIB)/curl/httpcache.c $(LIB)/curl/headers.c $(LIB)/crypto/crypto.c \
  $(LIB)/base64/base64.c $(LIB)/sqlite/sqlite.c $(LIB)/js/lexer.c $(LIB)/js/parser.c $(LIB)/js/ast.c \
  $(LIB)/js/value.c $(LIB)/js/interp.c $(LIB)/js/builtins.c $(LIB)/js/bigint.c $(LIB)/math/fmt.c \
  $(LIB)/gc/gc.c scripts/host_math.c $(LIB)/opt.c $(LIB)/mutex.c
MAIN_SRCS := $(COMMON_SRCS) $(LIB)/mime/mime.c $(LIB)/gfx/scrollview.c $(LIB)/gfx/textfield.c $(LIB)/zlib/crc32.c kernel/crc32c.c $(LIB)/stun/stun.c $(LIB)/ntp/ntp.c \
  $(LIB)/font/font.c $(LIB)/font/woff.c $(LIB)/mediactl/session.c $(LIB)/mp4/mp4.c $(wildcard $(LIB)/rar/*.c) $(LIB)/brotli/decode.c \
  drivers/usb/hid_desc.c $(LIB)/png/png.c $(LIB)/zlib/inflate.c $(LIB)/webp/webp.c $(LIB)/webp/vp8.c $(LIB)/gif/gif.c $(LIB)/imganim/imganim.c \
  $(wildcard $(LIB)/canvas2d/*.c) $(LIB)/font/font.c
BIND_SRCS := $(COMMON_SRCS) $(BROWSER)/html_entities.c $(LIB)/mime/mime.c $(BROWSER)/js/dom_query.c $(BROWSER)/features/cors/cors.c $(BROWSER)/blobstore.c $(BROWSER)/features/canvas/canvas.c \
  $(wildcard $(LIB)/canvas2d/*.c) $(LIB)/font/font.c $(LIB)/zlib/crc32.c $(LIB)/zlib/inflate.c $(LIB)/zlib/deflate.c

MAIN_SUITES := $(sort $(patsubst tests/%_test.c,%,$(shell grep -l '^\#define OWOS_SUITE_TEST$$' tests/*_test.c)))
BIND_SUITES := $(sort $(patsubst tests/%_test.c,%,$(shell grep -l '^\#define OWOS_SUITE_BINDINGS$$' tests/*_test.c)))

# Per-suite compile flags
EXTRA_permissions := -DPERM_DB='"$(RUN)/permissions.db"'
EXTRA_tz          := -DTZ_CONFIG_PATH='"$(RUN)/timezone"'
EXTRA_vary        := -DCACHE_DIR='"$(RUN)/vary-cache"'
EXTRA_btrfs       := $(HOST_DEFS)

# Objects whose sources are rebuilt with test-specific defines or a fake system layer
IMPLS := $(B)/impl/permissions.o $(B)/impl/cache.o $(B)/impl/vary_fs.o $(B)/impl/tz.o $(B)/impl/desktop.o \
         $(B)/impl/browser_js_host.o $(B)/impl/test_runner.o $(B)/impl/brotli_dict.o
BINDINGS_PARTS := bindings events navigator cssom runner clipboard blob canvas canvaspath canvasimg xhr realtime timers webworker webcomponents dialog forms url fetchapi navigation elementapi documentapi xpath namespace shadow streams
BIND_IMPLS := $(BINDINGS_PARTS:%=$(B)/bind/impl/%.o) $(B)/bind/impl/jar.o $(B)/bind/impl/clienthints.o \
              $(B)/bind/impl/permissions.o $(B)/bind/impl/host.o $(B)/impl/test_runner.o

MAIN_OBJS := $(MAIN_SUITES:%=$(B)/suite/%.o) $(IMPLS) $(MAIN_SRCS:%.c=$(B)/obj/%.o)
BIND_OBJS := $(BIND_SUITES:%=$(B)/bind/suite/%.o) $(BIND_IMPLS) $(BIND_SRCS:%.c=$(B)/obj/%.o)

# Flags live in this file, so editing it rebuilds every object
$(MAIN_OBJS) $(BIND_OBJS): scripts/tests.mk

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
	$(call COMPILE,$(SUITE_FLAGS))

$(B)/impl/permissions.o: $(BROWSER)/features/permissions/permissions.c
	$(call COMPILE,-DPERM_DB='"$(RUN)/permissions.db"')
$(B)/impl/cache.o: $(BROWSER)/features/cache/cache.c
	$(call COMPILE,-DCACHE_DIR='"$(RUN)/vary-cache"' -Dmkdir=vary_mkdir -Dunlink=vary_unlink -Dreaddir=vary_readdir -include tests/vary_fs.h)
$(B)/impl/vary_fs.o: tests/vary_fs.c
	@mkdir -p $(@D) && $(CC) -std=gnu11 -Itests -DCACHE_DIR='"$(RUN)/vary-cache"' -MMD -MP -c $< -o $@
$(B)/impl/tz.o: $(LIB)/tz/tz.c
	$(call COMPILE,-DTZ_CONFIG_PATH='"$(RUN)/timezone"')
$(B)/impl/desktop.o: $(LIB)/desktop/desktop.c
	$(call COMPILE,-Dreaddir=desktop_test_readdir -include tests/desktop_fs.h)
$(B)/impl/browser_js_host.o: tests/browser_js_host.c
	$(call COMPILE,)
$(B)/impl/test_runner.o: tests/test_runner.c
	$(call COMPILE,)

# The Brotli static dictionary is an objcopy'd blob on the target; embed it as a C array here
$(B)/impl/brotli_dict.c: $(LIB)/brotli/dictionary.bin
	@mkdir -p $(@D) && { echo 'const unsigned char _binary_user_lib_brotli_dictionary_bin_start[] = {'; \
	  od -An -v -tu1 $< | awk '{ for (i = 1; i <= NF; i++) printf "%s,", $$i; print "" }'; echo '};'; } > $@
$(B)/impl/brotli_dict.o: $(B)/impl/brotli_dict.c
	$(call COMPILE,)

# Suites that link the real browser bindings get their own binary
$(BINDINGS_PARTS:%=$(B)/bind/impl/%.o): $(B)/bind/impl/%.o: $(BROWSER)/js/%.c
	$(call COMPILE,-include tests/bindings_host.h -Drealloc=poison_realloc)
$(B)/bind/impl/jar.o: $(BROWSER)/features/jar/jar.c
	$(call COMPILE,-include tests/bindings_host.h -Drealloc=poison_realloc)
$(B)/bind/impl/clienthints.o: $(BROWSER)/features/clienthints/clienthints.c
	$(call COMPILE,-include tests/bindings_host.h -Drealloc=poison_realloc)
$(B)/bind/impl/permissions.o: $(BROWSER)/features/permissions/permissions.c
	$(call COMPILE,-DPERM_DB='"$(RUN)/bind-permissions.db"')
$(B)/bind/impl/host.o: tests/bindings_host.c
	$(call COMPILE,)

-include $(shell find $(B) -name '*.d' 2>/dev/null)
