# kernel/arch/aarch64/build.mk: bare AArch64 support layer; the board layout comes from the device tree
# (QEMU virt, MediaTek MT6765 phones such as the Samsung Galaxy A12, Tegra X1 boards such as the Nintendo Switch).
# The image is position independent and runs at any 4 KiB aligned address the loader picks; on the Switch that is
# hekate chaining L4T U-Boot, which places it at 0x80080000 with booti or at the address a boot script loads it to.
#   make ARCH=aarch64         build owos-aarch64.bin
#   make ARCH=aarch64 run     boot it headless, serial on the terminal
#   make ARCH=aarch64 check   boot it and fail unless the self-test passes (A64_GIC=3 for a GICv3 machine)

CROSS ?= $(if $(MACOS),aarch64-none-elf-)
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
READELF := $(CROSS)readelf
KERNEL  := owos-aarch64.bin
A64ELF  := owos-aarch64.elf
QEMU    := qemu-system-aarch64
A64DIR  := kernel/arch/aarch64
# Linux-targeted GCCs default to outline atomics, which call into libgcc.
A64FLAGS := -mgeneral-regs-only -mstrict-align -mno-outline-atomics -mcpu=cortex-a57 -std=gnu11 -ffreestanding -fpie \
            -fno-stack-protector -fno-tree-loop-distribute-patterns -O$(OPT_LEVEL) -Wall -Wextra -MMD -MP $(VERSION_DEF) -I$(A64DIR) -Ikernel/arch/common -Icommon -Ikernel
A64_EL2 ?= on
A64_SMP ?= 4
A64_GIC ?= 2
QEMU_A64 := $(QEMU) -M virt,virtualization=$(A64_EL2),gic-version=$(A64_GIC) -cpu cortex-a57 -smp $(A64_SMP) -m 256 -display none -serial stdio -kernel $(KERNEL)

$(call require_tool,$(CC),Install an aarch64-none-elf toolchain or override CROSS)

A64USER := user/aarch64
A64TESTS := tests/aarch64
A64UFLAGS := -mgeneral-regs-only -mstrict-align -mno-outline-atomics -mcpu=cortex-a57 -std=gnu11 -ffreestanding -fno-pie \
             -fno-stack-protector -fno-tree-loop-distribute-patterns -O$(OPT_LEVEL) -Wall -Wextra -Icommon -I$(A64USER)
A64PROGS := hello segv nx
A64UELFS := $(addprefix $(A64TESTS)/,$(addsuffix .elf,$(A64PROGS)))

KOBJS := $(addprefix $(A64DIR)/,boot.o vectors.o switch.o arch.o main.o fdt.o memmap.o plat.o pwrap.o clk.o gpio.o i2c.o i2c_mtk.o tegra_car.o tegra_gpio.o i2c_tegra.o max77620.o fbcon.o uart.o gic.o vm.o thread.o smp.o elf.o task.o syscall.o console.o mem.o kstring.o pmm.o heap.o)
OBJS := $(KOBJS) $(addprefix $(A64TESTS)/,selftest.o snippets.o programs.o)

$(A64DIR)/%.o: $(A64DIR)/%.S
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64DIR)/%.o: $(A64DIR)/%.c $(A64DIR)/arch.h
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64DIR)/%.o: kernel/arch/common/%.c
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64DIR)/%.o: kernel/%.c
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64TESTS)/selftest.o: $(A64TESTS)/selftest.c $(A64DIR)/arch.h
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64TESTS)/snippets.o: $(A64TESTS)/snippets.S
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64TESTS)/programs.o: $(A64TESTS)/programs.S $(A64UELFS)
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64PROGS:%=$(A64TESTS)/%.o): $(A64TESTS)/%.o: $(A64TESTS)/%.c $(A64USER)/rt.h
	$(CC) $(A64UFLAGS) -c $< -o $@

$(A64USER)/%.o: $(A64USER)/%.c $(A64USER)/rt.h
	$(CC) $(A64UFLAGS) -c $< -o $@

$(A64USER)/%.o: $(A64USER)/%.S
	$(CC) $(A64UFLAGS) -c $< -o $@

$(A64TESTS)/%.elf: $(A64TESTS)/%.o $(A64USER)/start.o $(A64USER)/rt.o $(A64USER)/user.ld
	$(LD) --no-warn-rwx-segments -nostdlib -z max-page-size=4096 -T $(A64USER)/user.ld -o $@ $< $(A64USER)/start.o $(A64USER)/rt.o

# boot.S applies R_AARCH64_RELATIVE entries and skips R_AARCH64_NONE, so any other dynamic relocation fails the link.
$(A64ELF): $(OBJS) $(A64DIR)/linker.ld
	$(LD) --no-warn-rwx-segments -nostdlib -pie --no-dynamic-linker -z notext -T $(A64DIR)/linker.ld -o $@.tmp $(OBJS)
	@if $(READELF) -rW $@.tmp | awk '/^ *[0-9a-f]+ +[0-9a-f]+ +R_/ && $$3 != "R_AARCH64_RELATIVE" && $$3 != "R_AARCH64_NONE" { bad = 1; print } END { exit !bad }'; then \
	  echo "$@: dynamic relocations other than R_AARCH64_RELATIVE" >&2; rm -f $@.tmp; exit 1; fi
	@mv $@.tmp $@

$(KERNEL): $(A64ELF)
	$(OBJCOPY) -O binary $< $@

run: $(KERNEL)
	$(QEMU_A64)

check: $(KERNEL)
	@out=$$(timeout 20 $(QEMU_A64) 2>&1); echo "$$out"; echo "$$out" | grep -qF 'AARCH64 SELFTEST PASS'

-include $(OBJS:.o=.d)

.SECONDARY:
