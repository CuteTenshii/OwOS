# run.mk: disk images, the QEMU run/debug targets, and the bootable ISO/USB.

DISK     := disk.img
SATADISK := sata.img
NVMEDISK := nvme.img
USBDISK  := usb.img
SDDISK   := sd.img
SCSIDISK := scsi.img
UFSDISK  := ufs.img
CDISK    := cd.iso

# Which NIC QEMU attaches. Default rtl8139 (QEMU's classic card); override to
# exercise an Intel driver, e.g. `make run NIC=e1000` (82540EM) or
# `make run NIC=igb` (82576, the igb-family proxy for the I210/I211). The
# rtl8139, e1000, igb, and vmxnet3 (`NIC=vmxnet3`) drivers are all built in and the kernel keeps whichever
# is present. (`-device e1000e`/82574L is PCIe and may need `-machine q35`.)
NIC ?= rtl8139

# Which SCSI adapter QEMU attaches: virtio-scsi-pci (default) or pvscsi (VMware PVSCSI), e.g. `make run SCSI=pvscsi`.
SCSI ?= virtio-scsi-pci

# How many CPU cores QEMU exposes to the guest. The kernel discovers them via the
# ACPI MADT; override with e.g. `make run SMP=8`. Set SMP=1 for the old uniprocessor
# behaviour.
SMP ?= 4

# QEMU host audio backend. Differs by OS: PipeWire on Linux, CoreAudio on macOS
# (QEMU has no pipewire backend there; see `qemu-system-i386 -audiodev help`). Override
# with e.g. `make run QEMU_AUDIODEV=pa` (PulseAudio) or `none` to mute.
ifeq ($(shell uname -s),Darwin)
  QEMU_AUDIODEV ?= coreaudio
else
  QEMU_AUDIODEV ?= pipewire
endif

# Hardware acceleration. Without it QEMU interprets every instruction (TCG), and
# every framebuffer write takes the dirty-tracking softmmu slow path, measured
# ~11x the cost of a back-buffer write, which is what makes the desktop feel like
# a slideshow. KVM (Linux) / HVF (macOS) run the guest on the host CPU with
# hardware dirty tracking, ~10-50x faster overall. The `:tcg` suffix is a
# graceful fallback when /dev/kvm is absent or unprivileged. Override with
# `make run ACCEL=tcg` to force pure emulation.
ifeq ($(shell uname -s),Darwin)
  ACCEL ?= hvf:tcg
else
  ACCEL ?= kvm:tcg
endif

# A 64 MiB raw disk image for the filesystem; persists across reboots. owofs
# sizes itself to the disk's capacity (capped at 64 MiB in fs.c).
$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1M count=64 status=none

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

$(SCSIDISK):
	dd if=/dev/zero of=$(SCSIDISK) bs=1M count=16 status=none

$(UFSDISK):
	dd if=/dev/zero of=$(UFSDISK) bs=1M count=16 status=none

# A FAT16-formatted image presented as a USB flash drive (qemu-xhci +
# usb-storage), so the xHCI mass-storage driver (kernel/xhci.c) has a real stick
# to enumerate, mount at /fat, and eject. Delete usb.img to re-seed.
$(USBDISK):
	dd if=/dev/zero of=$(USBDISK) bs=1M count=16 status=none
	mkfs.fat -F 16 -n OWOSUSB $(USBDISK) >/dev/null
	printf 'hello from a USB stick\n' > /tmp/owos_usb.txt
	mmd   -i $(USBDISK) ::photos
	mcopy -i $(USBDISK) /tmp/owos_usb.txt ::READID.TXT
	mcopy -i $(USBDISK) /tmp/owos_usb.txt ::photos/NOTES.TXT
	rm -f /tmp/owos_usb.txt

# A FAT16-formatted image presented as an SD card behind an SDHCI controller
# (sdhci-pci + sd-card), so the SD/MMC driver (kernel/sd.c) has a real card to
# bring up and mount at /media. Delete sd.img to re-seed.
$(SDDISK):
	dd if=/dev/zero of=$(SDDISK) bs=1M count=64 status=none
	mkfs.fat -F 16 -n OWOSSD $(SDDISK) >/dev/null
	printf 'hello from an SD card\n' > /tmp/owos_sd.txt
	mmd   -i $(SDDISK) ::DCIM
	mcopy -i $(SDDISK) /tmp/owos_sd.txt ::READSD.TXT
	mcopy -i $(SDDISK) /tmp/owos_sd.txt ::DCIM/NOTES.TXT
	rm -f /tmp/owos_sd.txt

# A real ISO 9660 disc image (with a Joliet name tree, -J) presented as an ATAPI
# CD-ROM, so the CD/DVD driver (kernel/atapi.c) and ISO filesystem
# (kernel/iso9660.c) have a disc to read and mount at /media. Built with
# xorrisofs (from xorriso, already a dependency of grub-mkrescue). Delete cd.iso
# to re-seed.
$(CDISK):
	rm -rf /tmp/owos_cd && mkdir -p /tmp/owos_cd/docs
	printf 'hello from an ISO 9660 CD-ROM\n' > /tmp/owos_cd/HELLO.TXT
	printf 'OwOS reads ISO 9660 and Joliet long names.\n' > '/tmp/owos_cd/A Long Disc File.txt'
	printf 'nested on the disc\n' > /tmp/owos_cd/docs/README.TXT
	xorrisofs -J -R -V OWOSCD -o $(CDISK) /tmp/owos_cd >/dev/null 2>&1
	rm -rf /tmp/owos_cd

# disk.img stays the IDE boot disk (owofs). The SATA disk hangs off an AHCI
# controller and the NVMe disk off an emulated PCIe SSD, so all three storage
# drivers find hardware to drive. A USB flash drive hangs off an xHCI controller;
# the CD-ROM rides QEMU's secondary IDE channel (-cdrom).
STORAGE := -hda $(DISK) \
	-device ich9-ahci,id=ahci \
	-drive id=satadisk,file=$(SATADISK),if=none,format=raw \
	-device ide-hd,drive=satadisk,bus=ahci.0 \
	-drive id=nvmedisk,file=$(NVMEDISK),if=none,format=raw \
	-device nvme,drive=nvmedisk,serial=owos-nvme0 \
	-device $(SCSI),id=scsi0 \
	-drive id=scsidisk,file=$(SCSIDISK),if=none,format=raw \
	-device scsi-hd,drive=scsidisk,bus=scsi0.0 \
	-device ufs,id=ufs0,serial=owos-ufs0 \
	-drive id=ufsdisk,file=$(UFSDISK),if=none,format=raw \
	-device ufs-lu,drive=ufsdisk,bus=ufs0,lun=0 \
	-device qemu-xhci,id=xhci \
	-drive id=usbdisk,file=$(USBDISK),if=none,format=raw \
	-device usb-storage,bus=xhci.0,drive=usbdisk \
	-device sdhci-pci,id=sdhci \
	-drive id=sddisk,file=$(SDDISK),if=none,format=raw \
	-device sd-card,drive=sddisk \
	-cdrom $(CDISK)

# Host<->guest clipboard: QEMU's built-in vdagent chardev over virtio-serial (no spice needed). The
# host UI must support clipboard sharing (GTK, Cocoa, VNC); the guest side is drivers/bus/vdagent.c.
# The guest agent (drivers/bus/qemuga.c) listens on QGA_SOCK, e.g.
#   echo '{"execute":"guest-ping"}' | nc -U /tmp/owos-qga.sock
QGA_SOCK ?= /tmp/owos-qga.sock
VDAGENT := -chardev qemu-vdagent,id=vdagent,clipboard=on \
	-device virtio-serial-pci,disable-legacy=on \
	-device virtserialport,chardev=vdagent,name=com.redhat.spice.0,nr=1 \
	-chardev socket,id=qga,path=$(QGA_SOCK),server=on,wait=off \
	-device virtserialport,chardev=qga,name=org.qemu.guest_agent.0,nr=2

# USB_HID=1 also attaches a USB keyboard, mouse and tablet to the xHCI bus, so the USB HID driver gets exercised
# (`make run USB_HID=1`); off by default so the PS/2 input path stays the only one.
ifeq ($(USB_HID),1)
  STORAGE += -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0 -device usb-tablet,bus=xhci.0
endif

# How the kernel image is handed to QEMU. The built-in `-kernel` multiboot loader
# only loads 32-bit ELF ("Cannot load x86-64 image, give a 32bit one"), so the
# 64-bit kernel is booted through GRUB on the ISO instead (multiboot2 loads
# ELF64). The ISO carries only the kernel; disk.img etc. still attach as devices.
ifeq ($(ARCH),x86_64)
  BOOTARG := -cdrom $(ISO)
  BOOTDEP := $(ISO)
else
  BOOTARG := -kernel $(KERNEL)
  BOOTDEP := $(KERNEL)
endif

run: $(BOOTDEP) $(DISK) $(SATADISK) $(NVMEDISK) $(SCSIDISK) $(UFSDISK) $(USBDISK) $(SDDISK) $(CDISK)
	$(QEMU) $(BOOTARG) -smp $(SMP) -m 1024 $(STORAGE) -serial stdio \
		-netdev user,id=net0,hostfwd=tcp::1445-:445,hostfwd=tcp::2222-:22,hostfwd=tcp:127.0.0.1:9222-:9222 -device $(NIC),netdev=net0 \
		-audiodev $(QEMU_AUDIODEV),id=snd0 -machine pcspk-audiodev=snd0,accel=$(ACCEL) \
		-device sb16,audiodev=snd0 \
		-device intel-hda -device hda-output,audiodev=snd0 \
		-usb -device usb-tablet \
		$(VDAGENT)

# Headless: no window, console comes out on the terminal via COM1.
debug: $(BOOTDEP) $(DISK) $(SATADISK) $(NVMEDISK) $(SCSIDISK) $(UFSDISK) $(USBDISK) $(SDDISK) $(CDISK)
	$(QEMU) $(BOOTARG) -smp $(SMP) -m 1024 -display none $(STORAGE) -serial stdio \
		-machine accel=$(ACCEL) \
		-netdev user,id=net0,hostfwd=tcp::1445-:445,hostfwd=tcp::2222-:22,hostfwd=tcp:127.0.0.1:9222-:9222 -device $(NIC),netdev=net0 \
		-usb -device usb-tablet \
		$(VDAGENT)

# --- bootable ISO ----------------------------------------------------------
# QEMU's -kernel is its own Multiboot loader; real VMs (VMware, VirtualBox,
# bare metal) need a bootloader on the media. Wrap the kernel in GRUB and emit
# a hybrid El Torito image that boots on BOTH legacy BIOS and UEFI: grub-mkrescue
# automatically adds a UEFI boot entry (an EFI/BOOT/BOOTX64.EFI in a FAT image)
# and makes the ISO GPT-hybrid whenever the host has the *-efi GRUB platform
# modules installed alongside i386-pc. Needs grub-mkrescue + xorriso + mtools.
#
# grub.cfg: `insmod all_video` is load-bearing: without a video driver loaded,
# GRUB cannot honor the kernel's Multiboot framebuffer request and dies with
# "no suitable video mode found" on real hardware/UEFI (it pulls in efi_gop /
# efi_uga on UEFI and vbe / vga on BIOS). We deliberately do NOT set
# gfxpayload=keep: that would force the kernel to inherit GRUB's console mode
# (often 24bpp), overriding the kernel's multiboot2 32bpp framebuffer request,
# which fb_adopt needs (it only handles 32-bpp XRGB).
#
# The kernel command line carries `verbose` so the ISO (the real-VM / bare-metal
# path) shows the bring-up log on screen by default, since the boot output is the
# first thing you want when a real machine misbehaves. The QEMU -kernel targets
# stay quiet (no `verbose` in their -append); the log is always in the journal.
$(ISO): $(KERNEL)
	mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL) $(ISODIR)/boot/$(KERNEL)
	printf 'set timeout=0\nset default=0\ninsmod all_video\nset gfxmode=1920x1080x32,1366x768x32,1280x1024x32,1024x768x32,800x600x32,auto\nmenuentry "OwOS" {\n\tinsmod all_video\n\tmultiboot2 /boot/$(KERNEL) verbose\n\tboot\n}\n' \
		> $(ISODIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(ISO) $(ISODIR)

iso: $(ISO)

# Sanity-check the ISO under QEMU the way a real VM boots it: CD only, no
# -kernel and no -hda, so missing-disk paths surface here and not in VMware.
# This exercises the legacy-BIOS (SeaBIOS) boot path.
run-iso: $(ISO)
	$(QEMU) -cdrom $(ISO) -serial stdio

# Boot the SAME ISO through UEFI (OVMF firmware) to prove the hybrid image's
# UEFI entry works; this is the path a modern VM/laptop firmware takes. OVMF is
# 64-bit; the EFI GRUB it launches still loads our 32-bit multiboot kernel.
# Override OVMF_CODE=/path if your distro puts the firmware elsewhere. Homebrew's
# qemu ships it under $(brew --prefix qemu)/share/qemu on macOS; most Linux distros
# put it in /usr/share/edk2 or /usr/share/OVMF.
ifeq ($(shell uname -s),Darwin)
  QEMU_SHARE := $(shell brew --prefix qemu 2>/dev/null)/share/qemu
  OVMF_CODE ?= $(QEMU_SHARE)/edk2-x86_64-code.fd
  OVMF_VARS ?= $(QEMU_SHARE)/edk2-i386-vars.fd
else
  OVMF_CODE ?= /usr/share/edk2/x64/OVMF_CODE.4m.fd
  OVMF_VARS ?= /usr/share/edk2/x64/OVMF_VARS.4m.fd
endif
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
	@test "$$(uname -s)" = "Linux" || { echo "refusing: 'make usb' uses Linux /sys/block to verify the target is removable; on macOS flash owos.iso manually with 'diskutil list' to find the disk, 'diskutil unmountDisk /dev/diskN', then 'sudo dd if=$(ISO) of=/dev/rdiskN bs=4m'"; exit 1; }
	@test -n "$(USB)" || { echo "refusing: name the target, e.g. make usb USB=/dev/sdc"; exit 1; }
	@test -b "$(USB)" || { echo "refusing: $(USB) is not a block device"; exit 1; }
	@dev=$$(basename $(USB)); \
	 test "$$(cat /sys/block/$$dev/removable 2>/dev/null)" = "1" || \
		{ echo "refusing: $(USB) is not a removable device (pass the whole disk, not a partition)"; exit 1; }
	@! mount | grep -q "^$(USB)" || { echo "refusing: $(USB) has a mounted partition; unmount it first"; exit 1; }
	@dev=$$(basename $(USB)); \
	 echo "About to ERASE $(USB) [$$(cat /sys/block/$$dev/device/vendor 2>/dev/null) $$(cat /sys/block/$$dev/device/model 2>/dev/null), $$(($$(cat /sys/block/$$dev/size)/2097152)) GiB] and write $(ISO)."
	echo "Press Ctrl-C to abort, or Enter to continue."; read
	dd if=$(ISO) of=$(USB) bs=4M oflag=direct status=progress conv=fsync
	sync
	@echo "done: $(ISO) written to $(USB); set BIOS/UEFI to boot from it"
