# Makefile for OwOS — a freestanding 32-bit multiboot kernel.
#
#   make        build the kernel image (owos.bin)
#   make run    build, then boot it in QEMU
#   make clean  remove build artifacts

KERNEL   := owos.bin
ISO      := owos.iso
ISODIR   := isodir
DISK     := disk.img
SATADISK := sata.img
NVMEDISK := nvme.img

CC      := gcc
# Freestanding: no host headers/libc, no PIE, no stack protector, no red zone.
# -mno-sse/-mno-mmx: the kernel never enables SSE (CR4.OSFXSR), so GCC must not
# auto-vectorize (e.g. a 16-byte init) into xmm/mmx instructions — those #UD.
CFLAGS  := -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
           -mno-sse -mno-mmx -Wall -Wextra -Ikernel
LDFLAGS := -m32 -ffreestanding -nostdlib -no-pie -T linker.ld

OBJS    := boot/boot.o kernel/kernel.o kernel/console.o kernel/gdt.o kernel/gdt_flush.o \
           kernel/idt.o kernel/isr.o kernel/irq.o kernel/irq_stubs.o \
           kernel/keyboard.o kernel/timer.o kernel/shell.o kernel/serial.o kernel/log.o \
           kernel/pmm.o kernel/paging.o kernel/heap.o kernel/kstring.o \
           kernel/sched.o kernel/switch.o \
           kernel/ata.o kernel/ahci.o kernel/nvme.o kernel/blk.o kernel/fs.o kernel/fat.o kernel/hfs.o kernel/ext.o kernel/pci.o kernel/usb.o kernel/xhci.o \
           kernel/nic.o kernel/rtl8139.o kernel/e1000.o kernel/net.o kernel/tcp.o kernel/crypto.o kernel/tls.o \
           kernel/url.o kernel/zip.o kernel/gzip.o kernel/png.o kernel/screenshot.o \
           kernel/bencode.o kernel/torrent.o kernel/tracker.o kernel/peer.o \
           kernel/speaker.o kernel/sb16.o \
           kernel/cpu.o kernel/rtc.o kernel/neofetch.o kernel/power.o kernel/battery.o \
           kernel/firmware.o kernel/smbios.o kernel/aml.o kernel/edid.o \
           kernel/syscall.o kernel/loader.o kernel/pipe.o kernel/services.o \
           kernel/smb.o \
           kernel/fb.o kernel/mtrr.o kernel/mouse.o kernel/wm.o kernel/uwin.o kernel/crash.o \
           kernel/apps.o \
           kernel/edit_blob.o kernel/curl_blob.o \
           kernel/editor_blob.o kernel/about_blob.o \
           kernel/clock_blob.o kernel/files_blob.o kernel/browser_blob.o \
           kernel/imgview_blob.o kernel/crashme_blob.o kernel/term_blob.o

# Which NIC QEMU attaches. Default rtl8139 (QEMU's classic card); override to
# exercise the Intel driver, e.g. `make run NIC=e1000` (82540EM) — both the
# RTL8139 and e1000 drivers are built in and the kernel picks whichever is
# present. (`-device e1000e`/82574L is PCIe and may need `-machine q35`.)
NIC ?= rtl8139

.PHONY: all run debug iso run-iso run-iso-uefi usb clean distclean

all: $(KERNEL)

$(KERNEL): $(OBJS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# --- user-mode programs ----------------------------------------------------
# Built freestanding against the tiny user lib, linked flat at 0x200000, then
# embedded into the kernel as an object.
UCFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -Iuser/lib -Wall
ULIB    := user/lib/start.o user/lib/lib.o user/lib/gfx.o
# Optional libs, linked only into the programs that use them (so a tiny program
# like hello doesn't carry the 32 KiB inflate window or the decoder buffers).
INFLATE := user/lib/inflate.o
UPNG    := user/lib/upng.o
URL     := user/lib/url.o     # shared URL parse/resolve (curl + browser)

user/lib/start.o: user/lib/start.S
	$(CC) $(UCFLAGS) -c $< -o $@
user/lib/lib.o: user/lib/lib.c
	$(CC) $(UCFLAGS) -c $< -o $@

user/%.o: user/%.c
	$(CC) $(UCFLAGS) -c $< -o $@

user/%.elf: user/%.o $(ULIB) user/user.ld
	ld -m elf_i386 -T user/user.ld -o $@ $< $(ULIB)

# curl speaks HTTP, so it links the shared URL lib (overrides the pattern above).
user/curl.elf: user/curl.o $(URL) $(ULIB) user/user.ld
	ld -m elf_i386 -T user/user.ld -o $@ user/curl.o $(URL) $(ULIB)

# The browser is a multi-file user program (user/browser/*.c), linked flat.
# It pulls in the streaming inflate lib for gzip/deflate HTTP responses and the
# PNG decoder so image/* pages render instead of showing as raw bytes.
BROWSER_OBJS := user/browser/main.o user/browser/fetch.o user/browser/html.o
user/browser/%.o: user/browser/%.c
	$(CC) $(UCFLAGS) -c $< -o $@
user/browser.elf: $(BROWSER_OBJS) $(UPNG) $(INFLATE) $(URL) $(ULIB) user/user.ld
	ld -m elf_i386 -T user/user.ld -o $@ $(BROWSER_OBJS) $(UPNG) $(INFLATE) $(URL) $(ULIB)

# The image viewer links the streaming PNG decoder (which uses inflate).
user/imgview.elf: user/imgview.o $(UPNG) $(INFLATE) $(ULIB) user/user.ld
	ld -m elf_i386 -T user/user.ld -o $@ user/imgview.o $(UPNG) $(INFLATE) $(ULIB)

# Embed each ELF as an object; gives _binary_user_<name>_elf_{start,end}.
kernel/%_blob.o: user/%.elf
	objcopy -I binary -O elf32-i386 -B i386 $< $@

# A 16 MiB raw disk image for the filesystem; persists across reboots.
$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1M count=16 status=none

# The SATA (AHCI) scratch disk doubles as a FAT volume so the read-only FAT
# driver (kernel/fat.c) has something real to mount. Formatted FAT16 and seeded
# with a small tree (incl. a long filename and a subdirectory) via mtools, no
# root/loop-mount needed. Delete sata.img to re-seed. The NVMe disk stays blank.
$(SATADISK):
	dd if=/dev/zero of=$(SATADISK) bs=1M count=16 status=none
	mkfs.fat -F 16 -n OWOSFAT $(SATADISK) >/dev/null
	printf 'hello from a FAT16 volume\n' > /tmp/owos_fat_hello.txt
	printf 'OwOS reads FAT12/16/32, long names and all.\n' > /tmp/owos_fat_long.txt
	mmd   -i $(SATADISK) ::docs
	mcopy -i $(SATADISK) /tmp/owos_fat_hello.txt ::HELLO.TXT
	mcopy -i $(SATADISK) /tmp/owos_fat_long.txt  '::A Long File Name.txt'
	mcopy -i $(SATADISK) /tmp/owos_fat_hello.txt ::docs/README.TXT
	rm -f /tmp/owos_fat_hello.txt /tmp/owos_fat_long.txt
$(NVMEDISK):
	dd if=/dev/zero of=$(NVMEDISK) bs=1M count=16 status=none

# A FAT16-formatted image presented as a USB flash drive (qemu-xhci +
# usb-storage), so the xHCI mass-storage driver (kernel/xhci.c) has a real stick
# to enumerate, mount at /fat, and eject. Delete usb.img to re-seed.
USBDISK := usb.img
$(USBDISK):
	dd if=/dev/zero of=$(USBDISK) bs=1M count=16 status=none
	mkfs.fat -F 16 -n OWOSUSB $(USBDISK) >/dev/null
	printf 'hello from a USB stick\n' > /tmp/owos_usb.txt
	mmd   -i $(USBDISK) ::photos
	mcopy -i $(USBDISK) /tmp/owos_usb.txt ::READID.TXT
	mcopy -i $(USBDISK) /tmp/owos_usb.txt ::photos/NOTES.TXT
	rm -f /tmp/owos_usb.txt

# disk.img stays the IDE boot disk (owofs). The SATA disk hangs off an AHCI
# controller and the NVMe disk off an emulated PCIe SSD, so all three storage
# drivers find hardware to drive. A USB flash drive hangs off an xHCI controller.
STORAGE := -hda $(DISK) \
	-device ich9-ahci,id=ahci \
	-drive id=satadisk,file=$(SATADISK),if=none,format=raw \
	-device ide-hd,drive=satadisk,bus=ahci.0 \
	-drive id=nvmedisk,file=$(NVMEDISK),if=none,format=raw \
	-device nvme,drive=nvmedisk,serial=owos-nvme0 \
	-device qemu-xhci,id=xhci \
	-drive id=usbdisk,file=$(USBDISK),if=none,format=raw \
	-device usb-storage,bus=xhci.0,drive=usbdisk

run: $(KERNEL) $(DISK) $(SATADISK) $(NVMEDISK) $(USBDISK)
	qemu-system-i386 -kernel $(KERNEL) $(STORAGE) -serial stdio \
		-netdev user,id=net0,hostfwd=tcp::1445-:445 -device $(NIC),netdev=net0 \
		-audiodev pipewire,id=snd0 -machine pcspk-audiodev=snd0 \
		-device sb16,audiodev=snd0 \
		-usb -device usb-tablet

# Headless: no window, console comes out on the terminal via COM1.
debug: $(KERNEL) $(DISK) $(SATADISK) $(NVMEDISK) $(USBDISK)
	qemu-system-i386 -kernel $(KERNEL) -display none $(STORAGE) -serial stdio \
		-netdev user,id=net0,hostfwd=tcp::1445-:445 -device $(NIC),netdev=net0 \
		-usb -device usb-tablet

# --- bootable ISO ----------------------------------------------------------
# QEMU's -kernel is its own Multiboot loader; real VMs (VMware, VirtualBox,
# bare metal) need a bootloader on the media. Wrap the kernel in GRUB and emit
# a hybrid El Torito image that boots on BOTH legacy BIOS and UEFI: grub-mkrescue
# automatically adds a UEFI boot entry (an EFI/BOOT/BOOTX64.EFI in a FAT image)
# and makes the ISO GPT-hybrid whenever the host has the *-efi GRUB platform
# modules installed alongside i386-pc. Needs grub-mkrescue + xorriso + mtools.
#
# grub.cfg: `insmod all_video` is load-bearing — without a video driver loaded,
# GRUB cannot honor the kernel's Multiboot framebuffer request and dies with
# "no suitable video mode found" on real hardware/UEFI (it pulls in efi_gop /
# efi_uga on UEFI and vbe / vga on BIOS). We deliberately do NOT set
# gfxpayload=keep: that would force the kernel to inherit GRUB's console mode
# (often 24bpp), overriding the kernel's multiboot2 32bpp framebuffer request,
# which fb_adopt needs (it only handles 32-bpp XRGB).
#
# The kernel command line carries `verbose` so the ISO (the real-VM / bare-metal
# path) shows the bring-up log on screen by default — the boot output is the
# first thing you want when a real machine misbehaves. The QEMU -kernel targets
# stay quiet (no `verbose` in their -append); the log is always in the journal.
$(ISO): $(KERNEL)
	mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL) $(ISODIR)/boot/$(KERNEL)
	printf 'set timeout=0\nset default=0\ninsmod all_video\nset gfxmode=1920x1080x32,1366x768x32,1280x1024x32,1024x768x32,800x600x32,auto\nmenuentry "OwOS" {\n\tinsmod all_video\n\tmultiboot2 /boot/$(KERNEL) verbose\n\tboot\n}\n' \
		> $(ISODIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(ISODIR)

iso: $(ISO)

# Sanity-check the ISO under QEMU the way a real VM boots it: CD only, no
# -kernel and no -hda, so missing-disk paths surface here and not in VMware.
# This exercises the legacy-BIOS (SeaBIOS) boot path.
run-iso: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -serial stdio

# Boot the SAME ISO through UEFI (OVMF firmware) to prove the hybrid image's
# UEFI entry works — this is the path a modern VM/laptop firmware takes. OVMF is
# 64-bit; the EFI GRUB it launches still loads our 32-bit multiboot kernel.
# Override OVMF_CODE=/path if your distro puts the firmware elsewhere.
OVMF_CODE ?= /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS ?= /usr/share/edk2/x64/OVMF_VARS.4m.fd
run-iso-uefi: $(ISO)
	cp $(OVMF_VARS) /tmp/owos_ovmf_vars.fd
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,unit=0,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,unit=1,file=/tmp/owos_ovmf_vars.fd \
		-cdrom $(ISO) -serial stdio

# --- flash the bootable ISO to a USB stick ---------------------------------
# Write owos.iso onto a removable USB drive with dd; the hybrid image boots on
# real BIOS/UEFI machines. This ERASES THE WHOLE DRIVE, so it refuses to run
# unless you name the device explicitly AND the kernel reports it as removable
# (so a typo can't nuke a fixed disk), then asks for one final confirmation.
#   make usb USB=/dev/sdc
USB ?=
usb: $(ISO)
	@test -n "$(USB)" || { echo "refusing: name the target, e.g. make usb USB=/dev/sdc"; exit 1; }
	@test -b "$(USB)" || { echo "refusing: $(USB) is not a block device"; exit 1; }
	@dev=$$(basename $(USB)); \
	 test "$$(cat /sys/block/$$dev/removable 2>/dev/null)" = "1" || \
		{ echo "refusing: $(USB) is not a removable device (pass the whole disk, not a partition)"; exit 1; }
	@! mount | grep -q "^$(USB)" || { echo "refusing: $(USB) has a mounted partition; unmount it first"; exit 1; }
	@dev=$$(basename $(USB)); \
	 echo "About to ERASE $(USB) [$$(cat /sys/block/$$dev/device/vendor 2>/dev/null) $$(cat /sys/block/$$dev/device/model 2>/dev/null), $$(($$(cat /sys/block/$$dev/size)/2097152)) GiB] and write $(ISO)."
	@printf 'Type YES to continue: '; read ans; test "$$ans" = "YES" || { echo "aborted"; exit 1; }
	dd if=$(ISO) of=$(USB) bs=4M oflag=direct status=progress conv=fsync
	sync
	@echo "done: $(ISO) written to $(USB) — set BIOS/UEFI to boot from it"

clean:
	rm -f $(OBJS) $(KERNEL) $(ISO)
	rm -f user/*.o user/*.elf user/lib/*.o user/browser/*.o user/browser/*.elf
	rm -rf $(ISODIR)

# Also remove the disk images (wipes stored files).
distclean: clean
	rm -f $(DISK) $(SATADISK) $(NVMEDISK)
