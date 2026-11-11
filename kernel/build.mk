# kernel/build.mk — the kernel image itself: the object list and the final link.
# Program blobs (kernel/*_blob.o) are built by user/apps/build.mk; the font blobs too.

# Generic kernel compile rules. Defined here, not toolchain.mk, so they come after
# user/lib's `user/%.o` rule: macOS GNU Make 3.81 resolves overlapping pattern rules
# by definition order (3.82+/Linux picks the shortest stem), so the user rule must be
# seen first or kernel CFLAGS would wrongly compile user code.
%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# KERNEL (owos.bin / owos-x86_64.bin) is set per-ARCH in toolchain.mk.
ISO    := owos.iso
ISODIR := isodir

# Objects are emitted in place (kernel/foo.o), shared by both arches. The stamp stores
# the active compiler configuration, so switching ARCH or OPT_LEVEL rebuilds objects.
.PHONY: FORCE
FORCE:

$(ARCH_STAMP): FORCE
	@if [ ! -f $@ ] || [ "$$(cat $@)" != "$(ARCH):O$(OPT_LEVEL)" ]; then \
		rm -f .arch.*; \
		printf '%s\n' '$(ARCH):O$(OPT_LEVEL)' > $@; \
		sleep 1; \
		touch $@; \
	fi

OBJS := boot/boot.o kernel/kernel.o kernel/console.o kernel/gdt.o kernel/gdt_flush.o \
        kernel/idt.o kernel/isr.o kernel/irq.o kernel/irq_stubs.o \
        drivers/input/keyboard.o kernel/timer.o kernel/shell.o kernel/serial.o kernel/log.o \
        kernel/pmm.o kernel/pmm_multiboot.o kernel/paging.o kernel/heap.o kernel/kstring.o \
        kernel/uaccess.o \
        kernel/sched.o kernel/switch.o \
        drivers/storage/ata.o drivers/storage/ahci.o drivers/storage/nvme.o drivers/storage/sd.o drivers/storage/scsi.o drivers/storage/vscsi.o drivers/storage/pvscsi.o drivers/storage/ufs.o drivers/storage/atapi.o kernel/blk.o kernel/fs.o kernel/procfs.o kernel/sysfs.o kernel/users.o kernel/fat.o kernel/hfs.o kernel/ext.o kernel/btrfs.o kernel/lzo.o kernel/zstd.o kernel/iso9660.o kernel/ntfs.o drivers/bus/pci.o drivers/usb/usb.o drivers/usb/xhci.o drivers/usb/hub.o drivers/usb/hid.o drivers/usb/hid_desc.o drivers/usb/hid_pad.o drivers/usb/xbox.o drivers/usb/btusb.o drivers/usb/uvc.o drivers/usb/uvc_desc.o drivers/usb/usb3.o drivers/bt/bt_hci.o drivers/bt/bt_l2cap.o drivers/bt/bt_gatt.o drivers/bt/bt_avdtp.o drivers/bt/bt_hid.o drivers/bt/bt_aes.o drivers/bt/bt_smp.o drivers/bt/bt_p256.o drivers/bt/sbc.o \
        drivers/net/nic.o drivers/net/rtl8139.o drivers/net/e1000.o drivers/net/igb.o drivers/net/vmxnet3.o kernel/net.o kernel/tcp.o kernel/crypto.o kernel/entropy.o \
        kernel/crc32.o kernel/crc32c.o kernel/gzip.o \
        drivers/audio/sb16.o drivers/audio/hda.o drivers/audio/es1371.o \
        kernel/cpu.o kernel/smp.o kernel/smp_ap.o kernel/rtc.o kernel/wallclock.o drivers/acpi/power.o drivers/acpi/s3_wake.o drivers/acpi/battery.o \
        kernel/firmware.o kernel/smbios.o drivers/acpi/aml.o drivers/video/edid.o drivers/video/dpaux.o drivers/video/edidparse.o drivers/video/backlight.o drivers/video/blpwm.o \
        kernel/audio.o kernel/syscall.o kernel/syscall_env.o kernel/env.o kernel/syscall_process.o kernel/syscall_fs.o kernel/syscall_net.o kernel/syscall_window.o kernel/syscall_audio.o kernel/syscall_graphics.o kernel/syscall_io.o kernel/syscall_signal.o kernel/syscall_exec.o kernel/syscall_entropy.o kernel/loader.o kernel/pipe.o kernel/ipc.o \
        drivers/bus/vmware.o drivers/bus/vmtools.o drivers/bus/virtioserial.o drivers/bus/vdagent.o drivers/bus/qemuga.o drivers/input/vmmouse.o drivers/input/synaptics.o drivers/video/vmsvga.o drivers/video/virtiogpu.o \
        drivers/video/fb.o drivers/video/mtrr.o drivers/input/mouse.o kernel/uwin.o kernel/crash.o \
        kernel/edit_blob.o kernel/curl_blob.o \
        kernel/editor_blob.o kernel/about_blob.o \
        kernel/clock_blob.o kernel/calc_blob.o kernel/files_blob.o kernel/browser_blob.o \
        kernel/compositor_blob.o \
        kernel/imgview_blob.o kernel/pdfview_blob.o kernel/term_blob.o \
        kernel/neofetch_blob.o kernel/lspci_blob.o kernel/uname_blob.o \
        kernel/ping_blob.o kernel/dig_blob.o kernel/stun_blob.o kernel/upnpc_blob.o kernel/torrent_blob.o \
        kernel/player_blob.o kernel/settings_blob.o kernel/ssh_blob.o kernel/sshd_blob.o \
        kernel/clipd_blob.o kernel/cliphist_blob.o kernel/vmclipd_blob.o \
        kernel/notifyd_blob.o kernel/notifysend_blob.o kernel/trayd_blob.o \
        kernel/webp_blob.o kernel/json_blob.o kernel/base64_blob.o kernel/alac_blob.o kernel/aac_blob.o kernel/mpeg_blob.o kernel/h264_blob.o kernel/smbd_blob.o \
        kernel/dhcpcd_blob.o kernel/ntpd_blob.o kernel/mdnsd_blob.o kernel/mediad_blob.o kernel/mediactl_blob.o \
        kernel/cast_blob.o kernel/chmod_blob.o kernel/chown_blob.o kernel/unrar_blob.o kernel/archive_blob.o \
        kernel/init_blob.o kernel/sudo_blob.o kernel/sh_blob.o $(foreach a,$(DESKTOP_APPS),kernel/$(a)_desktop_blob.o) \
        kernel/fontstore.o kernel/appsyms.o \
        $(foreach i,$(ICON_NAMES),kernel/icon_$(subst -,_,$(i))_blob.o)

$(KERNEL): $(OBJS) $(LINKER)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc

# Force a rebuild of every object when the target ARCH changes (see ARCH_STAMP).
$(OBJS): $(ARCH_STAMP)

# Blob objects have explicit rules in user/apps/build.mk. Keep their generated
# object format tied to the same architecture stamp as compiled sources.
$(filter %_blob.o,$(OBJS)): $(ARCH_STAMP)

-include $(OBJS:.o=.d)
