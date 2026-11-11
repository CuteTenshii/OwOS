# kernel/arch/arm/build.mk: 32-bit ARMv7-A support layer for the QEMU virt machine; the board layout comes from the
# device tree.
#   make ARCH=arm             build owos-arm.bin
#   make ARCH=arm run         boot it headless, serial on the terminal
#   make ARCH=arm check       boot it and fail unless the self-test passes (ARM_HYP=on starts the CPU in HYP mode)
# Builds with clang and ld.lld by default; CROSS=arm-none-eabi- uses a GNU toolchain instead.

OPT_LEVEL  ?= 2
OWOS_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
VERSION_DEF  := -DOWOS_VERSION='"$(OWOS_VERSION)"'

CROSS ?=
ifeq ($(CROSS),)
  CC      := clang --target=armv7a-none-eabi
  LD      := ld.lld
  OBJCOPY := llvm-objcopy
  ARM_TOOLFLAGS :=
  ARM_LDFLAGS :=
else
  CC      := $(CROSS)gcc
  LD      := $(CROSS)ld
  OBJCOPY := $(CROSS)objcopy
  ARM_TOOLFLAGS := -fno-tree-loop-distribute-patterns
  ARM_LDFLAGS := --no-warn-rwx-segments
endif

ifeq ($(shell command -v $(firstword $(CC)) >/dev/null 2>&1 && echo y),)
  $(error C compiler '$(firstword $(CC))' not found. Install clang and lld, or set CROSS to an arm-none-eabi toolchain.)
endif

KERNEL   := owos-arm.bin
ARMELF   := owos-arm.elf
QEMU     := qemu-system-arm
ARMDIR   := kernel/arch/arm
ARMCOMMON := kernel/arch/common
ARMTESTS := tests/arm
ARMFLAGS := -mcpu=cortex-a15 -marm -mfloat-abi=soft -mno-unaligned-access -std=gnu11 -ffreestanding -fno-pie \
            -fno-stack-protector $(ARM_TOOLFLAGS) -O$(OPT_LEVEL) -Wall -Wextra -MMD -MP $(VERSION_DEF) \
            -I$(ARMDIR) -I$(ARMCOMMON) -Icommon -Ikernel
ARM_HYP ?= off
QEMU_ARM := $(QEMU) -M virt,virtualization=$(ARM_HYP) -cpu cortex-a15 -m 256 -display none -serial stdio \
            -kernel $(KERNEL)

KOBJS := $(addprefix $(ARMDIR)/,boot.o vectors.o switch.o arch.o mmu.o main.o plat.o uart.o gic.o thread.o mem.o \
           console.o aeabi.o fdt.o memmap.o kstring.o pmm.o heap.o)
OBJS := $(KOBJS) $(ARMTESTS)/selftest.o

$(ARMDIR)/%.o: $(ARMDIR)/%.S
	$(CC) $(ARMFLAGS) -c $< -o $@

$(ARMDIR)/%.o: $(ARMDIR)/%.c $(ARMDIR)/arch.h
	$(CC) $(ARMFLAGS) -c $< -o $@

$(ARMDIR)/%.o: $(ARMCOMMON)/%.c
	$(CC) $(ARMFLAGS) -c $< -o $@

$(ARMDIR)/%.o: kernel/%.c
	$(CC) $(ARMFLAGS) -c $< -o $@

$(ARMTESTS)/selftest.o: $(ARMTESTS)/selftest.c $(ARMDIR)/arch.h
	$(CC) $(ARMFLAGS) -c $< -o $@

$(ARMELF): $(OBJS) $(ARMDIR)/linker.ld
	$(LD) $(ARM_LDFLAGS) -nostdlib -T $(ARMDIR)/linker.ld -o $@ $(OBJS)

$(KERNEL): $(ARMELF)
	$(OBJCOPY) -O binary $< $@

run: $(KERNEL)
	$(QEMU_ARM)

check: $(KERNEL)
	@out=$$(timeout 20 $(QEMU_ARM) 2>&1); echo "$$out"; echo "$$out" | grep -qF 'ARM SELFTEST PASS'

-include $(OBJS:.o=.d)

.SECONDARY:
