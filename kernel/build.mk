# kernel/build.mk — the kernel image itself: the object list and the final link.
# Program blobs (kernel/*_blob.o) are built by user/apps/build.mk; the font blobs too.

KERNEL := owos.bin
ISO    := owos.iso
ISODIR := isodir

OBJS := boot/boot.o kernel/kernel.o kernel/console.o kernel/gdt.o kernel/gdt_flush.o \
        kernel/idt.o kernel/isr.o kernel/irq.o kernel/irq_stubs.o \
        drivers/input/keyboard.o kernel/timer.o kernel/shell.o kernel/serial.o kernel/log.o \
        kernel/pmm.o kernel/paging.o kernel/heap.o kernel/kstring.o \
        kernel/sched.o kernel/switch.o \
        drivers/storage/ata.o drivers/storage/ahci.o drivers/storage/nvme.o drivers/storage/sd.o drivers/storage/atapi.o kernel/blk.o kernel/fs.o kernel/procfs.o kernel/sysfs.o kernel/users.o kernel/fat.o kernel/hfs.o kernel/ext.o kernel/iso9660.o kernel/ntfs.o drivers/bus/pci.o drivers/usb/usb.o drivers/usb/xhci.o \
        drivers/net/nic.o drivers/net/rtl8139.o drivers/net/e1000.o drivers/net/igb.o kernel/net.o kernel/tcp.o kernel/crypto.o kernel/ed25519.o kernel/tls.o \
        kernel/url.o kernel/gzip.o kernel/png.o kernel/screenshot.o \
        drivers/audio/sb16.o drivers/audio/hda.o \
        kernel/cpu.o kernel/rtc.o drivers/acpi/power.o drivers/acpi/s3_wake.o drivers/acpi/battery.o \
        kernel/firmware.o kernel/smbios.o drivers/acpi/aml.o drivers/video/edid.o \
        kernel/syscall.o kernel/loader.o kernel/pipe.o kernel/ipc.o kernel/kclip.o \
        kernel/services/services.o kernel/services/svc_smbd.o kernel/services/svc_sshd.o \
        kernel/services/svc_dhcp.o kernel/services/svc_ntp.o \
        kernel/smb.o kernel/sshd.o \
        drivers/bus/vmware.o drivers/bus/vmtools.o drivers/input/vmmouse.o drivers/input/synaptics.o drivers/video/vmsvga.o \
        drivers/video/fb.o drivers/video/mtrr.o drivers/input/mouse.o kernel/wm.o kernel/uwin.o kernel/crash.o \
        kernel/apps.o \
        kernel/edit_blob.o kernel/curl_blob.o \
        kernel/editor_blob.o kernel/about_blob.o \
        kernel/clock_blob.o kernel/files_blob.o kernel/browser_blob.o \
        kernel/imgview_blob.o kernel/term_blob.o \
        kernel/neofetch_blob.o kernel/lspci_blob.o \
        kernel/ping_blob.o kernel/dig_blob.o kernel/torrent_blob.o \
        kernel/player_blob.o kernel/settings_blob.o kernel/ssh_blob.o \
        kernel/clipd_blob.o kernel/cliphist_blob.o \
        kernel/uifont_blob.o kernel/monofont_blob.o

$(KERNEL): $(OBJS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc
