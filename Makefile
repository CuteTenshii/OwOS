# Makefile for OwOS — a freestanding 32-bit multiboot kernel.
#
#   make        build the kernel image (owos.bin)
#   make run    build, then boot it in QEMU
#   make iso    wrap the kernel in GRUB -> bootable hybrid BIOS+UEFI owos.iso
#   make clean  remove build artifacts (keeps the disk images)
#
# The build is split by concern: each component's rules live in a build.mk inside
# that component's own directory; the two project-global pieces (toolchain flags,
# the run/disk targets) sit at the root. It stays a single non-recursive graph —
# these are `include`d (CWD stays the root), so every path remains root-relative.
# Include order matters: later fragments use variables the earlier ones define.
#   toolchain.mk            compiler/linker flags + generic kernel compile rules
#   user/lib/build.mk       libc + the per-library objects (UCFLAGS)
#   user/apps/build.mk      user/apps/{gui,cli} programs + their kernel blobs
#   kernel/build.mk         the kernel object list + the owos.bin link
#   run.mk                  disk images + the QEMU run/debug/iso/usb targets

.DEFAULT_GOAL := all
.PHONY: all run debug iso run-iso run-iso-uefi usb clean distclean

include toolchain.mk
include user/lib/build.mk
include user/apps/build.mk
include kernel/build.mk
include run.mk

all: $(KERNEL)

clean:
	rm -f $(OBJS) owos.bin owos-x86_64.bin $(ISO) .arch.*
	rm -f user/lib/*.o user/lib/*/*.o
	rm -f user/apps/cli/*.o user/apps/cli/*.elf
	rm -f user/apps/gui/*.o user/apps/gui/*.elf user/apps/gui/browser/*.o
	rm -rf $(ISODIR)

# Also remove the disk images (wipes stored files).
distclean: clean
	rm -f $(DISK) $(SATADISK) $(NVMEDISK) $(USBDISK) $(SDDISK) $(CDISK)
