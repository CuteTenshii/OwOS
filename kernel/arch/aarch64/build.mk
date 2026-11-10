# kernel/arch/aarch64/build.mk: bare AArch64 support layer for the QEMU virt machine.
#   make ARCH=aarch64         build owos-aarch64.bin
#   make ARCH=aarch64 run     boot it headless, serial on the terminal
#   make ARCH=aarch64 check   boot it and fail unless the self-test passes

HOST_OS    := $(shell uname -s)
OPT_LEVEL  ?= 2
OWOS_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
VERSION_DEF  := -DOWOS_VERSION='"$(OWOS_VERSION)"'

ifeq ($(HOST_OS),Darwin)
  CROSS ?= aarch64-none-elf-
else
  CROSS ?=
endif
DEVKITA64 := /opt/devkitpro/devkitA64/bin/
ifeq ($(shell command -v $(CROSS)gcc >/dev/null 2>&1 && echo y),)
  ifneq ($(wildcard $(DEVKITA64)aarch64-none-elf-gcc),)
    CROSS := $(DEVKITA64)aarch64-none-elf-
  endif
endif
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
KERNEL  := owos-aarch64.bin
A64ELF  := owos-aarch64.elf
QEMU    := qemu-system-aarch64
A64DIR  := kernel/arch/aarch64
A64FLAGS := -mgeneral-regs-only -mstrict-align -mcpu=cortex-a57 -std=gnu11 -ffreestanding -fno-pie \
            -fno-stack-protector -fno-tree-loop-distribute-patterns -O$(OPT_LEVEL) -Wall -Wextra $(VERSION_DEF) -I$(A64DIR) -Icommon -Ikernel
A64_EL2 ?= on
A64_SMP ?= 4
QEMU_A64 := $(QEMU) -M virt,virtualization=$(A64_EL2) -cpu cortex-a57 -smp $(A64_SMP) -m 256 -display none -serial stdio -kernel $(KERNEL)

ifeq ($(shell command -v $(CC) >/dev/null 2>&1 && echo y),)
  $(error C compiler '$(CC)' not found. Install an aarch64-none-elf toolchain or override CROSS.)
endif

OBJS := $(addprefix $(A64DIR)/,boot.o vectors.o switch.o arch.o main.o fdt.o vm.o thread.o smp.o task.o user.o console.o mem.o kstring.o pmm.o heap.o)

$(A64DIR)/%.o: $(A64DIR)/%.S
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64DIR)/%.o: $(A64DIR)/%.c $(A64DIR)/arch.h
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64DIR)/%.o: kernel/%.c
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64ELF): $(OBJS) $(A64DIR)/linker.ld
	$(LD) --no-warn-rwx-segments -nostdlib -T $(A64DIR)/linker.ld -o $@ $(OBJS)

$(KERNEL): $(A64ELF)
	$(OBJCOPY) -O binary $< $@

run: $(KERNEL)
	$(QEMU_A64)

check: $(KERNEL)
	@out=$$(timeout 20 $(QEMU_A64) 2>&1); echo "$$out"; echo "$$out" | ug -q 'AARCH64 SELFTEST PASS'
