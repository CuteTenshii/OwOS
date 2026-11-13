# Makefile for OwOS, a freestanding 32-bit multiboot kernel.
#
#   make        build the kernel image (owos.bin)
#   make run    build, then boot it in QEMU
#   make iso    wrap the kernel in GRUB -> bootable hybrid BIOS+UEFI owos.iso
#   make clean  remove build artifacts (keeps the disk images)
#
# The build is split by concern: each component's rules live in a build.mk inside
# that component's own directory; the two project-global pieces (toolchain flags,
# the run/disk targets) sit at the root. It stays a single non-recursive graph,
# these are `include`d (CWD stays the root), so every path remains root-relative.
# Include order matters: later fragments use variables the earlier ones define.
#   toolchain.mk            compiler/linker flags + generic kernel compile rules
#   user/lib/build.mk       libc + the per-library objects (UCFLAGS)
#   user/apps/build.mk      user/apps/<name>/ programs + the payload the kernel installs
#   kernel/build.mk         the kernel object list + the owos.bin link
#   run.mk                  disk images + the QEMU run/debug/iso/usb targets
# The ARM bring-up kernels replace all of these with kernel/arch/<arch>/build.mk.

.DEFAULT_GOAL := all

# Version baked in via -DOWOS_VERSION (kernel + user): git describe (tag, else short
# commit, +-dirty), "dev" outside git. Override: make OWOS_VERSION=1.2.
OWOS_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
VERSION_DEF  := -DOWOS_VERSION='"$(OWOS_VERSION)"'
OPT_LEVEL    ?= 2
# Non-empty on a macOS host.
MACOS        := $(filter Darwin,$(shell uname -s))

# $(call require_tool,cmd,hint): stop at parse time with an actionable message when cmd (its first word) is missing.
require_tool = $(if $(shell command -v $(firstword $(1)) >/dev/null 2>&1 && echo y),,$(error '$(firstword $(1))' not found. $(2)))
# $(call need_tool,cmd): recipe line failing only the target that needs cmd.
need_tool = @command -v $(1) >/dev/null 2>&1 || { echo "error: $(1) not found" >&2; exit 1; }
.PHONY: check all test format format-check tidy install-hooks run debug iso run-iso run-iso-uefi usb clean distclean

FORMAT_FILES := $(shell find drivers kernel user -type f \( -name '*.c' -o -name '*.h' \) \
	! -path 'kernel/font*.h' ! -path kernel/payload.c ! -path 'user/lib/gfx/font*.h' \
	! -path 'user/lib/mp3/minimp3.h' ! -path 'user/lib/tz/zones.h')
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
TIDY_FILES ?= $(shell find kernel drivers -type f -name '*.c')
# Kernel and driver sources only, in three sets with their own target flags: user space has its own include paths.
TIDY_SETS       := x86 a64 arm
TIDY_SRCS_x86    = $(filter-out kernel/arch/aarch64/% kernel/arch/arm/%,$(TIDY_FILES))
TIDY_SRCS_a64    = $(filter kernel/arch/aarch64/%,$(TIDY_FILES))
TIDY_SRCS_arm    = $(filter kernel/arch/arm/%,$(TIDY_FILES))
TIDY_TARGET_x86  = --target=$(if $(filter x86_64,$(ARCH)),x86_64,i386)-none-elf
TIDY_TARGET_a64 := --target=aarch64-none-elf -mgeneral-regs-only -Ikernel/arch/aarch64 -Ikernel/arch/common
TIDY_TARGET_arm := --target=armv7a-none-eabi -mcpu=cortex-a15 -marm -mfloat-abi=soft -Ikernel/arch/arm -Ikernel/arch/common
TIDY_INC_x86     = -Ikernel/arch/x86 $(addprefix -Idrivers/,$(DRVDIRS))

ifeq ($(ARCH),aarch64)
include kernel/arch/aarch64/build.mk
else ifeq ($(ARCH),arm)
include kernel/arch/arm/build.mk
else
include toolchain.mk
include user/lib/build.mk
include user/apps/build.mk
include kernel/build.mk
include run.mk
endif

all: $(KERNEL)

test:
	./tests/run-tests.sh $(T)

format:
	$(call need_tool,$(CLANG_FORMAT))
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	$(call need_tool,$(CLANG_FORMAT))
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

tidy:
	$(call need_tool,$(CLANG_TIDY))
	@set -e; $(foreach s,$(TIDY_SETS),$(if $(TIDY_SRCS_$(s)),\
		$(CLANG_TIDY) --quiet $(TIDY_SRCS_$(s)) -- $(TIDY_TARGET_$(s)) -std=gnu11 -ffreestanding -Wall -Wextra \
		$(VERSION_DEF) -Icommon -Ikernel $(TIDY_INC_$(s));))

install-hooks:
	$(call need_tool,lefthook)
	git config --unset core.hooksPath || true
	lefthook install

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) owos.bin owos-x86_64.bin owos-aarch64.bin owos-aarch64.elf owos-arm.bin owos-arm.elf $(ISO) .arch.*
	rm -f user/lib/*.o user/lib/*/*.o user/aarch64/*.o tests/aarch64/*.o tests/aarch64/*.elf kernel/arch/aarch64/*.d tests/aarch64/*.d
	rm -f kernel/arch/x86/*.o kernel/arch/x86/*.d
	rm -f kernel/arch/arm/*.o kernel/arch/arm/*.d tests/arm/*.o tests/arm/*.d
	find user/apps \( -name '*.o' -o -name '*.elf' \) -delete
	find user -name '*.d' -delete
	rm -f kernel/payload.c kernel/payload.list kernel/*_blob.o
	rm -rf $(ISODIR)

# Also remove the disk images (wipes stored files).
distclean: clean
	rm -rf build
	rm -f $(DISK_IMAGES)
