# toolchain.mk: compiler/linker flags and the generic kernel compile rules,
# shared by every other fragment.

# Target arch: i386 (default, 32-bit) or x86_64 (long mode). One source tree builds
# both; ARCH selects kernel toolchain, codegen, linker script, object format, output
# name and QEMU binary.
#   make             # 32-bit owos.bin
#   make ARCH=x86_64  # 64-bit owos-x86_64.bin
ARCH ?= i386
ifeq ($(filter $(ARCH),i386 x86_64),)
  $(error unsupported ARCH '$(ARCH)'; use i386 or x86_64)
endif

# Linux can use its native GCC with the explicit architecture flags below. macOS
# needs a cross-toolchain because its native compiler emits Mach-O, not ELF.
MACOS_CROSS_i386   := i686-elf-
MACOS_CROSS_x86_64 := x86_64-elf-
CROSS ?= $(if $(MACOS),$(MACOS_CROSS_$(ARCH)))

CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

# Fail early with an actionable message rather than a cryptic "elf_i386" error
# halfway through the link when the cross-toolchain isn't installed.
$(call require_tool,$(CC),Install the target ELF toolchain or override CROSS)

# User space is built for the kernel's ARCH (32-bit ELF under i386, 64-bit under x86_64), so it shares the toolchain.
UCC      := $(CC)
ULD      := $(LD)
UOBJCOPY := $(OBJCOPY)

# grub-mkrescue builds the bootable ISO (make iso); grub-mkimage the boot images the Installer writes to a disk,
# from the platform directories under GRUB_DIR.
GRUB_MKRESCUE ?= grub-mkrescue
GRUB_MKIMAGE ?= grub-mkimage
GRUB_DIR ?= /usr/lib/grub

# Per-arch knobs. KELF/KBFD are objcopy's output ELF format and BFD architecture
# for wrapping a user blob (or font) into a KERNEL object. LINKER is the kernel
# linker script; KERNEL the image name (so both arches' images coexist).
ifeq ($(ARCH),x86_64)
  # -mcmodel=large: the kernel is identity-mapped at low physical (1 MiB), not in
  # the top-2 GiB window -mcmodel=kernel assumes, so use 64-bit absolute addressing.
  # -mno-red-zone: interrupt handlers must not clobber the SysV red zone.
  ARCHFLAGS := -m64 -mcmodel=large -mno-red-zone
  LINKER    := kernel/arch/x86/linker64.ld
  KELF      := elf64-x86-64
  KBFD      := i386:x86-64
  QEMU      := qemu-system-x86_64
  KERNEL    := owos-x86_64.bin
  UARCHFLAGS := -m64
  ULDFLAGS  := -m elf_x86_64
else
  ARCHFLAGS := -m32
  LINKER    := kernel/arch/x86/linker.ld
  KELF      := elf32-i386
  KBFD      := i386
  QEMU      := qemu-system-i386
  KERNEL    := owos.bin
  UARCHFLAGS := -m32
  ULDFLAGS  := -m elf_i386
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
           -mno-sse -mno-mmx -Wall -Wextra -MMD -MP $(VERSION_DEF) -Icommon -Ikernel -Ikernel/arch/x86 $(addprefix -Idrivers/,$(DRVDIRS))
LDFLAGS := $(ARCHFLAGS) -ffreestanding -nostdlib -no-pie -T $(LINKER)

# The generic kernel compile rules (%.o) live in kernel/build.mk, not here: they must
# come after user/lib's user/%.o rule (macOS GNU Make 3.81 resolves overlapping
# pattern rules by definition order). Full why is in kernel/build.mk.
