# toolchain.mk — compiler/linker flags and the generic kernel compile rules,
# shared by every other fragment.

# Version string baked into the build: the current git tag, else the short commit
# hash when untagged (git describe --always), with a -dirty suffix when the tree
# has uncommitted changes. Overridable on the command line (`make OWOS_VERSION=1.2`)
# or env; falls back to "dev" outside a git checkout. Passed to both the kernel and
# user builds via -DOWOS_VERSION.
OWOS_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
VERSION_DEF  := -DOWOS_VERSION='"$(OWOS_VERSION)"'

# Target architecture: i386 (default, 32-bit) or x86_64 (64-bit long mode). The
# same source tree builds both; ARCH selects the kernel toolchain, code-gen flags,
# linker script, object/ELF format, output name and QEMU binary. The 32-bit build
# is the default and is unaffected by this switch. Override on the command line:
#   make                 # 32-bit owos.bin (default)
#   make ARCH=x86_64      # 64-bit owos-x86_64.bin
ARCH ?= i386
ifeq ($(filter $(ARCH),i386 x86_64),)
  $(error unsupported ARCH '$(ARCH)' — use i386 or x86_64)
endif

# Cross-compilation prefix for the KERNEL. The build needs a toolchain that emits
# ELF (gcc/binutils) for the target: the host's `ld`/`objcopy` must understand the
# matching `elf_i386`/`elf32-i386` (or `elf_x86_64`/`elf64-x86-64`) formats.
#
#   Linux: the native gcc/binutils do both with -m32/-m64 (install gcc-multilib),
#          so CROSS is empty and we use bare gcc/ld/objcopy.
#   macOS: the system "gcc"/"ld" are Apple Clang/ld and emit only Mach-O — they
#          cannot build the kernel. Use an ELF cross-toolchain instead:
#              brew install i686-elf-gcc i686-elf-binutils      (32-bit)
#              brew install x86_64-elf-gcc x86_64-elf-binutils  (64-bit)
#          and we default CROSS to the prefix matching ARCH. Override with
#          `make CROSS=...`.
ifeq ($(shell uname -s),Darwin)
  ifeq ($(ARCH),x86_64)
    CROSS ?= x86_64-elf-
  else
    CROSS ?= i686-elf-
  endif
endif
CROSS ?=

CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

# Fail early with an actionable message rather than a cryptic "elf_i386" error
# halfway through the link when the cross-toolchain isn't installed.
ifeq ($(shell command -v $(CC) >/dev/null 2>&1 && echo y),)
  $(error C compiler '$(CC)' not found. On macOS run: brew install i686-elf-gcc i686-elf-binutils (32-bit) or x86_64-elf-gcc x86_64-elf-binutils (64-bit), or set CROSS=. On Linux install gcc-multilib.)
endif

# User-space toolchain. User programs are ALWAYS 32-bit ELF, independent of the
# kernel ARCH: the kernel embeds each as an opaque binary blob (only the blob's
# wrapper-object format follows the kernel arch). x86_64-elf-gcc ships no 32-bit
# libgcc multilib, so even an x86_64 kernel build links user code with the i686
# toolchain. On Linux the native gcc -m32 covers it (UCROSS empty).
ifeq ($(shell uname -s),Darwin)
  UCROSS ?= i686-elf-
endif
UCROSS ?=
UCC      := $(UCROSS)gcc
ULD      := $(UCROSS)ld
UOBJCOPY := $(UCROSS)objcopy

# Per-arch knobs. KELF/KBFD are objcopy's output ELF format and BFD architecture
# for wrapping a user blob (or font) into a KERNEL object. LINKER is the kernel
# linker script; KERNEL the image name (so both arches' images coexist).
ifeq ($(ARCH),x86_64)
  # -mcmodel=large: the kernel is identity-mapped at low physical (1 MiB), not in
  # the top-2 GiB window -mcmodel=kernel assumes, so use 64-bit absolute addressing.
  # -mno-red-zone: interrupt handlers must not clobber the SysV red zone.
  ARCHFLAGS := -m64 -mcmodel=large -mno-red-zone
  LINKER    := linker64.ld
  KELF      := elf64-x86-64
  KBFD      := i386:x86-64
  QEMU      := qemu-system-x86_64
  KERNEL    := owos-x86_64.bin
else
  ARCHFLAGS := -m32
  LINKER    := linker.ld
  KELF      := elf32-i386
  KBFD      := i386
  QEMU      := qemu-system-i386
  KERNEL    := owos.bin
endif

# Freestanding: no host headers/libc, no PIE, no stack protector, no red zone.
# -mno-sse/-mno-mmx: the kernel never enables SSE (CR4.OSFXSR), so GCC must not
# auto-vectorize (e.g. a 16-byte init) into xmm/mmx instructions — those #UD.
# Device drivers live under drivers/<type>/; each subdir is added to the include
# path so the flat `#include "foo.h"` convention keeps resolving everywhere.
DRVDIRS := storage net audio usb input video acpi bus
CFLAGS  := $(ARCHFLAGS) -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
           -mno-sse -mno-mmx -Wall -Wextra $(VERSION_DEF) -Ikernel -Ikernel/services $(addprefix -Idrivers/,$(DRVDIRS))
LDFLAGS := $(ARCHFLAGS) -ffreestanding -nostdlib -no-pie -T $(LINKER)

# NOTE: the generic kernel compile rules (%.o: %.c / %.o: %.S) are defined in
# kernel/build.mk, NOT here. They must be defined *after* user/lib/build.mk's more
# specific `user/%.o` rule: GNU Make 3.81 (what macOS ships) picks the first
# matching pattern rule by definition order, not the shortest stem like 3.82+
# (Linux). Defining the generic catch-all here (first include) would make it
# shadow the user rule on macOS, compiling user code with kernel CFLAGS.
