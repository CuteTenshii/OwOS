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
#   user/apps/build.mk      user/apps/<name>/ programs + their kernel blobs
#   kernel/build.mk         the kernel object list + the owos.bin link
#   run.mk                  disk images + the QEMU run/debug/iso/usb targets

.DEFAULT_GOAL := all
.PHONY: all test format format-check tidy install-hooks run debug iso run-iso run-iso-uefi usb clean distclean

FORMAT_FILES := $(shell find boot drivers kernel user -type f \( -name '*.c' -o -name '*.h' \) \
	! -path 'kernel/font*.h' ! -path kernel/fontstore.c ! -path 'user/lib/gfx/font*.h' \
	! -path 'user/lib/mp3/minimp3.h' ! -path 'user/lib/tz/zones.h')
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
TIDY_TARGET = --target=$(if $(filter x86_64,$(ARCH)),x86_64,i386)-none-elf
TIDY_FILES ?= $(shell find kernel drivers -type f -name '*.c')

include toolchain.mk
include user/lib/build.mk
include user/apps/build.mk
include kernel/build.mk
include run.mk

all: $(KERNEL)

test:
	./scripts/run-tests.sh $(T)

format:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { echo "error: $(CLANG_FORMAT) not found" >&2; exit 1; }
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { echo "error: $(CLANG_FORMAT) not found" >&2; exit 1; }
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

# Kernel and driver sources only: user space has its own per-library include paths.
tidy:
	@command -v $(CLANG_TIDY) >/dev/null 2>&1 || { echo "error: $(CLANG_TIDY) not found" >&2; exit 1; }
	$(CLANG_TIDY) --quiet $(TIDY_FILES) -- $(TIDY_TARGET) -std=gnu11 -ffreestanding -Wall -Wextra $(VERSION_DEF) \
		-Icommon -Ikernel $(addprefix -Idrivers/,$(DRVDIRS))

install-hooks:
	@command -v lefthook >/dev/null 2>&1 || { echo "error: lefthook not found" >&2; exit 1; }
	git config --unset core.hooksPath || true
	lefthook install

clean:
	rm -f $(OBJS) owos.bin owos-x86_64.bin $(ISO) .arch.*
	rm -f user/lib/*.o user/lib/*/*.o
	find user/apps \( -name '*.o' -o -name '*.elf' \) -delete
	find user -name '*.d' -delete
	rm -rf $(ISODIR)

# Also remove the disk images (wipes stored files).
distclean: clean
	rm -rf build
	rm -f $(DISK) $(SATADISK) $(NVMEDISK) $(USBDISK) $(SDDISK) $(CDISK)
