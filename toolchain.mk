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
ifeq ($(filter $(ARCH),i386 x86_64),)
  $(error unsupported ARCH '$(ARCH)' — use i386 or x86_64)
endif

# Kernel cross-compile prefix. Linux: native gcc/binutils emit target ELF with
# -m32/-m64 (needs gcc-multilib), so CROSS is empty. macOS: Apple's gcc/ld emit only
# Mach-O, so use an ELF cross-toolchain (brew install {i686,x86_64}-elf-gcc/-binutils)
# — default the prefix per ARCH. Override with make CROSS=...
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

# User-space toolchain. User programs are ALWAYS 32-bit ELF regardless of kernel ARCH
# (the kernel embeds each as an opaque blob). x86_64-elf-gcc has no 32-bit libgcc, so
# even an x64 kernel links user code with the i686 toolchain; Linux gcc -m32 covers it.
ifeq ($(shell uname -s),Darwin)
  UCROSS ?= i686-elf-
endif
UCROSS ?=
UCC      := $(UCROSS)gcc
ULD      := $(UCROSS)ld
UOBJCOPY := $(UCROSS)objcopy

# grub-mkrescue builds the bootable ISO (make iso). Plain name on Linux; macOS
# Homebrew installs it arch-prefixed. Prefer plain, else a prefixed build on PATH.
GRUB_MKRESCUE ?= $(firstword $(foreach g,grub-mkrescue x86_64-elf-grub-mkrescue i686-elf-grub-mkrescue,$(shell command -v $(g) >/dev/null 2>&1 && echo $(g))) grub-mkrescue)

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
# (#UD). -O1 is required and capped: -O0 leaves the compositor's per-pixel helpers
# un-inlined (stutters); -O2 triple-faults at early boot (latent UB) — do not raise
# without re-verifying a clean boot. The two -fno guards are no-ops at -O1 but kept so
# a future -O2 attempt can't regress:
#   -fno-strict-aliasing                the kernel type-puns freely (mb2 tags, packet
#                                       and on-disk structs).
#   -fno-tree-loop-distribute-patterns  stops the loop-idiom pass rewriting kstring.c's
#                                       byte mem* into self-calls and the compositor's
#                                       dword copies into a slow byte memcpy.
# DRVDIRS: each driver subdir on -I so the flat #include "foo.h" convention resolves.
DRVDIRS := storage net audio usb input video acpi bus
CFLAGS  := $(ARCHFLAGS) -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
           -O1 -fno-strict-aliasing -fno-tree-loop-distribute-patterns \
           -mno-sse -mno-mmx -Wall -Wextra $(VERSION_DEF) -Ikernel -Ikernel/services $(addprefix -Idrivers/,$(DRVDIRS))
LDFLAGS := $(ARCHFLAGS) -ffreestanding -nostdlib -no-pie -T $(LINKER)

# The generic kernel compile rules (%.o) live in kernel/build.mk, not here: they must
# come after user/lib's user/%.o rule (macOS GNU Make 3.81 resolves overlapping
# pattern rules by definition order). Full why is in kernel/build.mk.
