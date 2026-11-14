# kernel/arch/aarch64/build.mk: the AArch64 kernel: this support layer (the board layout comes from the device tree:
# QEMU virt, MediaTek MT6765 phones such as the Samsung Galaxy A12, Tegra X1 boards such as the Nintendo Switch)
# under the shared kernel, with the user programs embedded as on x86.
# The image is position independent and runs at any 4 KiB aligned address the loader picks; on the Switch that is
# hekate chaining L4T U-Boot, which places it at 0x80080000 with booti or at the address a boot script loads it to.
#   make ARCH=aarch64         build owos-aarch64.bin
#   make ARCH=aarch64 run     boot it headless, the recovery console on the terminal
#   make ARCH=aarch64 check   boot it, run programs from the console and fail unless they work (A64_GIC=3: GICv3)

CROSS ?= $(if $(MACOS),aarch64-none-elf-)
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
READELF := $(CROSS)readelf
KERNEL  := owos-aarch64.bin
A64ELF  := owos-aarch64.elf
QEMU    := qemu-system-aarch64
A64DIR  := kernel/arch/aarch64
A64OBJ  := build/aarch64
DRVDIRS := storage net audio usb input video acpi bus bt
# Linux-targeted GCCs default to outline atomics, which call into libgcc.
A64FLAGS := -mgeneral-regs-only -mstrict-align -mno-outline-atomics -mcpu=cortex-a57 -std=gnu11 -ffreestanding -fpie \
            -fno-stack-protector -fno-tree-loop-distribute-patterns -fno-strict-aliasing -O$(OPT_LEVEL) -Wall -Wextra \
            -MMD -MP $(VERSION_DEF) -I$(A64DIR) -Ikernel/arch/common -Icommon -Ikernel $(addprefix -Idrivers/,$(DRVDIRS))
A64_EL2 ?= on
A64_SMP ?= 4
A64_GIC ?= 2
A64_MEM ?= 1024
QEMU_A64 := $(QEMU) -M virt,virtualization=$(A64_EL2),gic-version=$(A64_GIC) -cpu cortex-a57 -smp $(A64_SMP) \
            -m $(A64_MEM) -display none -serial stdio -kernel $(KERNEL)

$(call require_tool,$(CC),Install an aarch64-none-elf toolchain or override CROSS)

# User programs share the kernel's toolchain; user/lib/build.mk adds the target flags.
UCC         := $(CC)
ULD         := $(LD)
UOBJCOPY    := $(OBJCOPY)
UARCHFLAGS  :=
ULDFLAGS    :=
KELF        := elf64-littleaarch64
KBFD        := aarch64
APPSYMS_OBJ := $(A64OBJ)/appsyms.o
include user/lib/build.mk
include user/apps/build.mk

# The arch layer, then every shared kernel source but x86's boot, firmware tables and multiboot modules.
ARCH_OBJS := $(addprefix $(A64DIR)/,boot.o vectors.o switch.o arch.o main.o exc.o task.o paging.o fpu.o timer.o rtc.o \
               cpu.o serial.o power.o smp.o mem.o fdt.o memmap.o plat.o pwrap.o clk.o gpio.o i2c.o i2c_mtk.o \
               tegra_car.o tegra_gpio.o i2c_tegra.o max77620.o fbcon.o uart.o gic.o keyboard.o nodev.o)
SHARED_SRCS := $(filter-out kernel/kernel.c kernel/smbios.c kernel/bootmod.c kernel/payload.c kernel/fontstore.c, \
                 $(wildcard kernel/*.c))
SHARED_OBJS := $(patsubst kernel/%.c,$(A64OBJ)/kernel/%.o,$(SHARED_SRCS)) $(A64OBJ)/kernel/payload.o $(APPSYMS_OBJ)
OBJS := $(ARCH_OBJS) $(SHARED_OBJS)

$(A64DIR)/%.o: $(A64DIR)/%.S
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64DIR)/%.o: $(A64DIR)/%.c
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64DIR)/%.o: kernel/arch/common/%.c
	$(CC) $(A64FLAGS) -c $< -o $@

$(A64OBJ)/kernel/%.o: kernel/%.c
	@mkdir -p $(@D)
	$(CC) $(A64FLAGS) -c $< -o $@

# boot.S applies R_AARCH64_RELATIVE entries and skips R_AARCH64_NONE, so any other dynamic relocation fails the link.
$(A64ELF): $(OBJS) $(A64DIR)/linker.ld
	$(LD) --no-warn-rwx-segments -nostdlib -pie --no-dynamic-linker -z notext -T $(A64DIR)/linker.ld -o $@.tmp $(OBJS) \
	  $(shell $(CC) -print-libgcc-file-name)
	@if $(READELF) -rW $@.tmp | awk '/^ *[0-9a-f]+ +[0-9a-f]+ +R_/ && $$3 != "R_AARCH64_RELATIVE" && $$3 != "R_AARCH64_NONE" { bad = 1; print } END { exit !bad }'; then \
	  echo "$@: dynamic relocations other than R_AARCH64_RELATIVE" >&2; rm -f $@.tmp; exit 1; fi
	@mv $@.tmp $@

$(KERNEL): $(A64ELF)
	$(OBJCOPY) -O binary $< $@

run: $(KERNEL)
	$(QEMU_A64)

# Boot to init, run a program from the recovery console, then type into an interactive sh: it runs uname with its
# output piped back and written to a file, and base64 reads that file. Fails unless each printed what it should.
A64_CHECK_LOG := $(A64OBJ)/check.log
check: $(KERNEL)
	@A64_GIC=$(A64_GIC) scripts/qemu-test.sh --arch aarch64 --smp $(A64_SMP) --boot-wait 15 --gap 6 --drain 2 \
	  --log $(A64_CHECK_LOG) -- "uname -m" "sh" "uname -m > m" "base64 m" "exit" >/dev/null 2>&1; \
	cat $(A64_CHECK_LOG); \
	grep -qF 'init: pid 1 up' $(A64_CHECK_LOG) || { echo "check: init did not start" >&2; exit 1; }; \
	tr -d '\r' < $(A64_CHECK_LOG) | grep -qx 'aarch64' || { echo "check: uname -m did not print aarch64" >&2; exit 1; }; \
	grep -qF 'YWFyY2g2NAo=' $(A64_CHECK_LOG) || { echo "check: sh did not run uname > m, base64 m" >&2; exit 1; }; \
	echo "check: AArch64 userland PASS"

-include $(OBJS:.o=.d)

.SECONDARY:
