# toolchain.mk — compiler/linker flags and the generic kernel compile rules,
# shared by every other fragment.

# Version baked in via -DOWOS_VERSION (kernel + user): git describe (tag, else short
# commit, +-dirty), "dev" outside git. Override: make OWOS_VERSION=1.2.
OWOS_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
VERSION_DEF  := -DOWOS_VERSION='"$(OWOS_VERSION)"'

# Target arch: i386 (default, 32-bit) or x86_64 (long mode). One source tree builds
# both; ARCH selects kernel toolchain, codegen, linker script, object format, output
# name and QEMU binary.
#   make             # 32-bit owos.bin
#   make ARCH=x86_64  # 64-bit owos-x86_64.bin
ARCH ?= i386
OPT_LEVEL ?= 2
ARCH_STAMP := .arch.current
ifeq ($(filter $(ARCH),i386 x86_64),)
  $(error unsupported ARCH '$(ARCH)' — use i386 or x86_64)
endif

# Linux can use its native GCC with the explicit architecture flags below. macOS
# needs a cross-toolchain because its native compiler emits Mach-O, not ELF.
HOST_OS := $(shell uname -s)
ifeq ($(ARCH),x86_64)
  ifeq ($(HOST_OS),Darwin)
    CROSS ?= x86_64-elf-
  else
    CROSS ?=
  endif
else
  ifeq ($(HOST_OS),Darwin)
    CROSS ?= i686-elf-
  else
    CROSS ?=
  endif
endif

CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

# Fail early with an actionable message rather than a cryptic "elf_i386" error
# halfway through the link when the cross-toolchain isn't installed.
ifeq ($(shell command -v $(CC) >/dev/null 2>&1 && echo y),)
  $(error C compiler '$(CC)' not found. Install the target ELF toolchain or override CROSS.)
endif

# User-space programs are always 32-bit ELF because the kernel embeds them as blobs.
ifeq ($(HOST_OS),Darwin)
  UCROSS ?= i686-elf-
else
  UCROSS ?=
endif
UCC      := $(UCROSS)gcc
ULD      := $(UCROSS)ld
UOBJCOPY := $(UCROSS)objcopy

# grub-mkrescue builds the bootable ISO (make iso).
GRUB_MKRESCUE ?= grub-mkrescue

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

# Freestanding: no host libc/headers, no PIE/stack-protector/red-zone. -mno-sse/
# -mno-mmx: the kernel never enables SSE, so GCC must not auto-vectorize into xmm/mmx
# (#UD). O2 is the default: O0 leaves the compositor's per-pixel helpers
# un-inlined (stutters), and O2 has been boot-tested on the current kernel.
# Override with OPT_LEVEL=1 for diagnosis.
# The two -fno guards are no-ops at O1 but kept so
# a future -O2 attempt can't regress:
#   -fno-strict-aliasing                the kernel type-puns freely (mb2 tags, packet
#                                       and on-disk structs).
#   -fno-tree-loop-distribute-patterns  stops the loop-idiom pass rewriting kstring.c's
#                                       byte mem* into self-calls and the compositor's
#                                       dword copies into a slow byte memcpy.
# DRVDIRS: each driver subdir on -I so the flat #include "foo.h" convention resolves.
DRVDIRS := storage net audio usb input video acpi bus bt
CFLAGS  := $(ARCHFLAGS) -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
           -O$(OPT_LEVEL) -fno-strict-aliasing -fno-tree-loop-distribute-patterns \
           -mno-sse -mno-mmx -Wall -Wextra $(VERSION_DEF) -Icommon -Ikernel $(addprefix -Idrivers/,$(DRVDIRS))
LDFLAGS := $(ARCHFLAGS) -ffreestanding -nostdlib -no-pie -T $(LINKER)

# The generic kernel compile rules (%.o) live in kernel/build.mk, not here: they must
# come after user/lib's user/%.o rule (macOS GNU Make 3.81 resolves overlapping
# pattern rules by definition order). Full why is in kernel/build.mk.
