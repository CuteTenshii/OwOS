# OwOS

A 32-bit x86 operating system written from scratch in C and a little GAS
assembly - no host libc, no cross-compiler, no existing kernel underneath. QEMU
boots the freestanding kernel directly through its built-in Multiboot loader; a
GRUB ISO is also produced for real VMs and bare metal.

What started as a "does it boot" experiment now has a graphical desktop with a
window manager and mouse, user processes with `fork`/`exec`/`waitpid`, block
drivers for IDE/SATA/NVMe/SD/USB/CD-DVD, half a dozen filesystems, a from-scratch
TCP/IP + TLS network
stack with a real web browser on top, an SMB2 file server, a BitTorrent client,
USB host support, and Sound Blaster audio. With no framebuffer it falls back to
an 80×25 text console and everything still works.

```
make            # build owos.bin with the i686-elf target toolchain
make test       # compile and run the deterministic host test suite
make format     # format C sources and headers with clang-format
make format-check # verify C sources and headers are clang-format clean
make tidy       # run clang-tidy on kernel and driver sources
make install-hooks # install lefthook: clang-format + clang-tidy pre-commit, tests pre-push
make run        # QEMU: graphical window + COM1 mirrored to the terminal
make debug      # headless: console only, over COM1
make run NIC=e1000   # attach the Intel 82540EM instead of the default RTL8139
```

---

## Build & run

You need the target ELF toolchain `i686-elf-gcc` and `i686-elf-binutils`, plus
`qemu-system-i386`. Building the bootable ISO additionally needs `grub-mkrescue`,
`xorriso`, and `mtools`; the seeded FAT test disks need `mkfs.fat` (dosfstools).

The default build uses the `i686-elf-` toolchain. The 64-bit build uses
`x86_64-elf-` for both the kernel and the embedded user programs.
With [Homebrew](https://brew.sh), install the target toolchains and runtime tools:

  ```sh
  brew install i686-elf-gcc i686-elf-binutils   # the kernel toolchain
  brew install qemu mtools dosfstools xorriso    # run + seeded test disks
  ```


| Target | What it does |
| --- | --- |
| `make` | Build `owos.bin` (a freestanding 32-bit ELF linked at 1 MiB). |
| `make run` | Boot in QEMU with a graphical window; COM1 mirrored to the terminal. Adds Sound Blaster 16, the PC speaker, and a USB tablet. |
| `make debug` | Headless - no window, the console comes out on the terminal over COM1, so a whole session can be driven and captured as text. |
| `make test` | Compile and run the deterministic host-side tests in a temporary directory. |
| `make install-hooks` | Enable the repository-managed pre-commit and pre-push hooks. |
| `make iso` | Wrap the kernel in GRUB and emit a hybrid BIOS+UEFI `owos.iso`. |
| `make run-iso` | Boot the ISO on legacy BIOS (SeaBIOS), CD only, the way a real VM would. |
| `make run-iso-uefi` | Boot the same ISO through UEFI (OVMF firmware). |
| `make usb USB=/dev/sdX` | Flash `owos.iso` to a removable stick (guarded - it refuses anything that isn't a removable whole-disk, and asks for confirmation, because it erases the drive). |
| `make clean` | Remove build artifacts (keeps the disk images). |
| `make distclean` | Also wipe the disk images. |

### 64-bit (x86_64) build

The same source tree also builds a 64-bit (long-mode) kernel: `make ARCH=x86_64`
produces `owos-x86_64.bin`, an ELF64 image that boots into long mode (the boot
stub sets up PAE paging + LME and far-jumps to 64-bit code). The default build is
unchanged 32-bit. On macOS this needs the matching cross-toolchain
(`brew install x86_64-elf-gcc x86_64-elf-binutils`). The user programs follow `ARCH`:
the 64-bit kernel runs 64-bit ELF programs with SSE floating point, and the syscall ABI
stays the same 32-bit words on both, since every user address lies below 4 GiB.
Switching `ARCH` rebuilds the objects automatically (a `.arch.*` stamp guards it).

Note QEMU's built-in `-kernel` multiboot loader only loads 32-bit ELF, so
`make run ARCH=x86_64` boots the 64-bit kernel through GRUB on the ISO instead
(GRUB's multiboot2 loads ELF64) - which means the `iso` toolchain (Linux-only) is
required to run it.

On Linux there is **no cross-compiler**: the kernel is built with the host
`gcc -m32 -ffreestanding -nostdlib` and linked at 1 MiB by `kernel/arch/x86/linker.ld` (on macOS the
`i686-elf-` cross-toolchain above plays that role). QEMU's `-kernel` is itself a
Multiboot loader, so nothing else is needed there. The ISO exists only for the
targets that genuinely need a bootloader on the media - VMware, VirtualBox, bare
metal - and carries GRUB for both BIOS and UEFI.

`make run`/`debug` auto-create the disks and devices the drivers exercise: an
IDE disk (`disk.img`, the owofs boot volume), a SATA disk on an AHCI controller,
an NVMe SSD, a FAT-formatted USB stick on an xHCI controller, an SD card on an
SDHCI controller, an ISO 9660 `cd.iso` in an ATAPI CD-ROM, a SLIRP user-mode NIC,
and (for `run`) Sound Blaster 16 + a USB tablet. `disk.img` starts blank, so OwOS
runs live from RAM until you install it there with the Install OwOS app; from then on,
files persist across reboots.

> **Don't pass `-vga std`.** It leaves the card in a graphics mode where the
> 0xB8000 text window is unmapped and nothing renders. `make run` uses QEMU's
> default adapter on purpose so the framebuffer path works.

---

## What's inside

### Boot & firmware

`kernel/arch/x86/boot.S` carries **two** Multiboot headers: Multiboot1 (for QEMU `-kernel`,
which hands us the info struct directly) and Multiboot2 (for GRUB on both BIOS
and UEFI, whose mb1 handoff page-faults under UEFI). On the mb2 path the kernel
walks the tag chain and synthesizes an mb1-shaped info struct, so the PMM and
framebuffer code need no per-format logic. Along the way it detects the firmware
type (BIOS vs UEFI), captures the ACPI RSDP, and records the SMBIOS entry point,
all things the legacy BIOS scans can't find under UEFI.

### Graphics & the desktop

A 32-bpp linear framebuffer, a PS/2 mouse, and a compositing window manager.
fb.c gets the framebuffer one of two ways: it programs QEMU's Bochs/DISPI VBE
registers (`-kernel`), or it **adopts** the framebuffer GRUB/real hardware hands
it. The 80×25 VGA console keeps running underneath as a character grid that the
compositor can render with an 8×16 CP437 font - that grid is the kernel recovery
console; the desktop terminal is a separate user program.

The window manager (`wm.c`) is a single kernel thread: draggable title bars,
raise-on-click z-order, focus, close boxes, a cursor, and a clock/taskbar with a
Start menu. Performance is real on hardware (QEMU hides it) - a cached back
buffer above 16 MiB, damage-rectangle-only presents, and a write-combining front
buffer took a laptop from 2 fps to smooth.

Desktop apps open from the Start menu. Only the About box is kernel-side;
everything else - the terminal (a self-contained shell), clock, files,
editor, browser, image viewer - is a **user program** that draws into
a shared-memory pixel buffer the compositor hands it over the `wm` IPC port
(`user/lib/win/`, `user/apps/compositor/wmserver.c`): a clock, a file
browser, a text editor, an image viewer, and a real web browser. A user app
that faults gets a crash window instead of taking down the kernel.

### Memory

The PMM is a bitmap frame allocator over the Multiboot memory map. Paging
identity-maps the low 16 MiB; above that, frames are allocated and mapped on
demand (the framebuffer back buffer, the tmpfs RAM disk). The heap is a
first-fit `kmalloc`/`kfree` in [4 MiB, 16 MiB). Each process gets its own page
directory: shared kernel/heap mappings plus a **private 64 MiB user window at
1 GiB**, backed on demand by high physical memory. The scheduler switches CR3
and the TSS kernel stack per task; every address space maps the kernel
identically, so kernel threads and interrupt handlers run under any CR3.

### Processes & scheduling

A preemptive round-robin scheduler switches tasks on every timer tick. Tasks are
either kernel threads (ring 0, shared address space) or user processes (ring 3,
private address space). The full lifecycle works:

- **`fork`** duplicates the caller - address space and trap frame copied, the
  child's `eax` forced to 0.
- **the run-image syscall** (`exec`) replaces the current image with a fresh ELF
  from disk, rewriting the trap frame and swapping CR3 before freeing the old
  address space.
- **`exit`** makes a zombie; the parent reaps it via `waitpid`.

Processes carry a **cwd**, so `curl -o out.txt` lands in the current directory.
Each has a file-descriptor table (0/1/2 = console); `pipe` creates a kernel ring
buffer, fds survive `fork`/`exec`, and the shell wires `a | b` pipelines
kernel-side. User programs are freestanding ELF (ELF32 on i386, ELF64 on x86_64)
linked at the user window and embedded into the kernel image; the loader parses the program headers into a
fresh address space.

### Storage & filesystems

Three drivers cover the common disk interfaces - a polled ATA PIO driver (IDE),
a polled AHCI driver (SATA, MMIO + DMA), and a minimal NVMe driver (PCIe SSD,
admin/I/O queues). `blk.c` picks **one** of them at boot to back **owofs**, a flat
512-byte-sector filesystem with a directory tree that persists across reboots: the
first disk (IDE → AHCI → NVMe) with an OwOS root partition in its GPT, else the
first present disk, whose whole-disk owofs from an older install still mounts.
Booting never formats a disk. Without owofs on one, OwOS runs live from a RAM disk,
and only the Install OwOS app writes a disk, after you confirm which one.

The Install OwOS app lays a disk out as GPT with a 1 MiB BIOS boot partition (GRUB's core
image), a 128 MiB FAT32 EFI system partition (GRUB for UEFI, `grub.cfg` and the
kernel) and an OwOS root partition over the rest, so an installed disk boots on both
BIOS and UEFI. It copies the kernel image it is running from, which the ISO's GRUB
loads a second time as a multiboot module; the installed kernel fills its empty root
partition on first boot. The GRUB images are built with `grub-mkimage` when the host
has GRUB for `i386-pc`, `i386-efi` and `x86_64-efi` (override `GRUB_DIR`); without
them the build succeeds but the Install OwOS app has nothing to install.

Removable volumes mount read/write or read-only at **`/media`** via `mount` /
`eject`:

- **FAT12/16/32** - full read **and** write (host `fsck`-clean).
- **HFS+** ("Mac OS Extended") - read-only.
- **ext2/3/4** - read-only (extent trees + indirect blocks).
- **ISO 9660 + Joliet** - read-only CD/DVD (long, mixed-case names).

USB flash drives are handled by the xHCI mass-storage path (BOT/SCSI) and SD/MMC
cards by an SDHCI driver, so a plugged-in stick or card shows up as a block device
and mounts the same way. A **CD/DVD** mounts too: a polled **ATAPI** driver tunnels
SCSI commands over the IDE bus (2048-byte sectors) and the ISO 9660 driver reads
the disc.

`storage` lists every detected device with model and capacity; `neofetch` shows
them too.

### Networking

A from-scratch stack, strictly layered:

- **Link layer** - a NIC dispatch layer (`nic.c`) probes the RTL8139 (QEMU's
  default), the Intel e1000 (8254x/82574/I217-I219; `make run NIC=e1000`), and
  the Intel igb family (I210/I211/82576; `make run NIC=igb`), and the VMware
  vmxnet3 (`make run NIC=vmxnet3`), keeping whichever is present. The rest of the stack is driver-agnostic.
- **IPv4 and IPv6** (`net.c`) - ARP, NDP with SLAAC, ICMP and ICMPv6 echo, UDP, TCP, DNS (A and AAAA), and a DHCP client.
- **TCP** (`tcp.c`) - active **and** passive open, several concurrent
  connections, SYN-accepted-in-IRQ with a daemon thread taking the connection.
- **TLS** (`tls.c` + `crypto.c`) - a client-only TLS 1.3 (1.2 fallback) with
  X25519, AES-128-GCM, and SHA-256/HMAC/HKDF, freestanding and allocation-free.
  The server certificate is **not** verified yet - deliberate: it resists
  passive eavesdropping, not an active MITM.

On top of that:

- `ping <host>`, `dig <host> [type]` (A/AAAA/CNAME/MX/TXT/NS…). DHCP (boot lease)
  and NTP (clock sync) are **services** - `services start ntp`,
  `services restart dhcp`.
- **`curl <url>`** - an HTTP/1.1 client (the only user-facing network API
  is four `net_sock_*` syscalls): `http://` or `https://`, `-i`, `-X`, `-o`.
- **A graphical web browser** - a desktop app that fetches over HTTP(S),
  inflates gzip/deflate, parses HTML, decodes PNG images, and renders the page,
  streaming downloads to `~/Downloads`.

### Services & the SMB server

A small service manager (`services.c`) runs headless background daemons from an
enum-indexed registry: a 1 Hz `beat`, a UDP echo server (`echod`, RFC 862), a
UDP daytime server (`daytimed`, RFC 867), a TCP echo server (`echotcp`), and
**`smbd`** - a read-only **SMB2 file server** that shares owofs to Linux
`smbclient` / `mount -t cifs` and Windows/KDE clients (verified byte-exact on
>64 KiB files). UDP daemons serve straight from the receive interrupt; TCP
daemons own an accept-loop thread. `make run`/`debug` forward host port 1445 to
the guest's 445 so `smbd` is reachable.

### BitTorrent client

A from-scratch BitTorrent stack, a `/bin/torrent` program over
`user/lib/torrent/`: a bencode parser, `.torrent` metainfo parsing with the
SHA-1 infohash, tracker announce, and the peer wire protocol - handshake, piece
download, and SHA-1 verification. `torrent get <file>` downloads a whole
single-file torrent to disk.

### Audio

The PC speaker plays square-wave tones (`beep`), and a Sound Blaster 16 driver
(8-bit PCM over ISA DMA) plays real sound: `play <tune>` for a built-in melody
(twinkle / megalovania / sweden) or `play <file.wav>` to stream a RIFF/WAVE file
from disk (any PCM 8/16-bit mono/stereo is down-converted; Ctrl+C stops it).

### USB

`lsusb` walks the USB host controllers - for UHCI (QEMU's PIIX3) it drives a
real control transfer to fetch device descriptors. xHCI (USB 3.x, every port on
modern laptops) is brought up at boot and kept persistent; it backs the USB
mass-storage path today, with phone tethering (an RNDIS/CDC driver registering
as a NIC) as the goal.

### Power, ACPI & sensors

`poweroff` performs ACPI S5 (walking RSDP → XSDT/RSDT → FADT → DSDT for `_S5`);
`restart` pulses the 8042. A tiny best-effort AML interpreter reads the battery
(`_BST`/`_BIF`) for the neofetch splash. These battery/EDID/SMBIOS paths are
mostly real-hardware-only (QEMU can't exercise a battery), and carry diagnostic
plumbing for debugging on a laptop.

### Niceties

CPUID identifies the processor, the CMOS RTC gives wall-clock time, and
`neofetch` shows it all next to an OwO logo - including a "Host:" line read from
SMBIOS/DMI and (on real Intel graphics) the monitor refresh rate over DDC/EDID.
Print Screen captures the whole framebuffer to a PNG in your home directory. The
shell keeps a command history (Up/Down) and supports in-line editing. The
keyboard layout switches at runtime (`loadkeys us|fr`), with US QWERTY and French
AZERTY (including AltGr) built in.

---

## Shell commands

The desktop **terminal is a self-contained shell** (`user/term.c`): it
does its own line editing, history (Up/Down), prompt, and command dispatch - no
kernel involvement beyond syscalls. **Ctrl+C** cancels the current line, or
aborts a running command. (The kernel keeps a minimal recovery console on COM1
for headless `make debug`.)

```
help  clear  uptime  sleep  meminfo  tasks  journal
ls  pwd  cd  cat  grep  write  cp  mv  rm  mkdir  rmdir  stat
zip  unzip  gzip  gunzip          (LZSS/DEFLATE codecs)
which  echo
storage  mount  eject  lspci  lsusb
ping  dig  curl  torrent
services                          (list/start/stop/restart/status; dhcp/ntp are services)
play  cpuinfo  uname  whoami  loadkeys  clip  date  neofetch
poweroff  restart
```

System info comes from `/proc` (`cat /proc/cpuinfo`, `/proc/meminfo`, …); kernel
actions go through `/proc` control nodes or a syscall. CLI tools
(`neofetch`/`curl`/`ping`/…) run as captured child processes rendered inline.

Any command's output can be piped into `grep` (`<cmd> | grep <pattern>`) or
redirected to a file (`<cmd> > <file>`). A bare non-builtin name runs the
matching `/bin` program, so `curl <url>`, `editor file.txt`, and friends just
work. Bundled programs: `edit`, `editor`, `curl`, `browser`, `imgview`,
`pdfview`, `files`, `clock`, `about`, `crashme`.

---

## Per-file layout

```
kernel/arch/x86/ - the x86 CPU layer, shared by i386 and x86_64; files that
          differ by word size come as _32/_64 pairs, picked by ARCH:

kernel/arch/x86/boot.S              Multiboot1+2 headers, entry point, stack setup
kernel/arch/x86/linker.ld, 64.ld    load the kernel at 1 MiB, multiboot header first
kernel/arch/x86/gdt.c gdt_flush_*.S GDT + lgdt/segment reload
kernel/arch/x86/idt.c isr*.S        CPU exception IDT + dispatcher (panic screen + COM1)
kernel/arch/x86/irq.c irq_*.S       PIC remap + hardware IRQ dispatch
kernel/arch/x86/switch_*.S          context switch and new-thread entry
kernel/arch/x86/fpu.c               per-task FPU state (x87 on i386, x87 + SSE on x86_64)
kernel/arch/x86/smp.c smp_ap_*.S    MADT topology and application-processor bring-up
kernel/arch/x86/paging_*.c          identity map, frame pool, per-process address spaces
kernel/arch/x86/timer.c             PIT (250 Hz), tick counter + sleep (IRQ0)
kernel/arch/x86/serial.c            polled COM1 UART (console mirror / logging)
kernel/arch/x86/rtc.c               CMOS real-time clock
kernel/arch/x86/cpu.c               CPUID vendor/brand

kernel/kernel.c    kernel_main - brings subsystems up in dependency order
kernel/console.c   text + framebuffer console funnel (was vga.c)
kernel/shell.c     COM1/text recovery console (the desktop shell is user/term.c)
kernel/crash.c     crash reporter (fault -> crash window)

kernel/pmm.c       physical frame allocator (bitmap over the mmap)
kernel/heap.c      kmalloc/kfree (first-fit free list)
kernel/kstring.c   freestanding memory primitives
kernel/sched.c     scheduler: kernel threads + user processes, fork, reaping
kernel/syscall.c   int 0x80 dispatch (write..net..win)
kernel/loader.c    ELF loader: load segments into a fresh address space
kernel/pipe.c      kernel pipe ring buffers
kernel/file.h      per-process fd table

kernel/shm.c       shared memory objects granted to other pids (window buffers)
kernel/ipc.c       named IPC ports; each channel knows the pid/uid at its other end
kernel/apps.c      desktop app registry / Start menu
kernel/screenshot.c Print Screen -> PNG capture worker
kernel/png.c       PNG decoder (over the DEFLATE engine)

kernel/blk.c       block-device selection for owofs (IDE>AHCI>NVMe) + /media mount
kernel/fs.c        owofs: minimal persistent filesystem
kernel/fat.c       FAT12/16/32 read+write driver (mounts at /media)
kernel/hfs.c       HFS+ read-only driver
kernel/ext.c       ext2/3/4 read-only driver
kernel/iso9660.c   ISO 9660 + Joliet read-only CD/DVD driver

kernel/net.c       IPv4/IPv6: ARP, NDP, ICMP, UDP/TCP, DHCP
kernel/tcp.c       concurrent TCP: client (connect) + server (listen/accept)
kernel/tls.c       client TLS 1.3/1.2 (for https)
kernel/crypto.c    SHA-256/HMAC/HKDF, AES-128-GCM, X25519
kernel/url.c       freestanding URL parser/builder
kernel/services.c  service manager: supervisor + headless daemons
kernel/smb.c       read-only SMB2 file server (smbd)

kernel/gzip.c      gzip/DEFLATE codec + CRC32 (png screenshot; user space: user/lib/zlib/deflate.c)
kernel/smbios.c    SMBIOS/DMI host name for neofetch
kernel/firmware.c  BIOS vs UEFI detection
kernel/neofetch.c  system-info splash

drivers/ - device drivers, grouped by type (each subdir is on the -I path,
          so cross-file includes stay flat: #include "fb.h", not a path):

drivers/bus/pci.c        PCI bus enumeration (config mechanism #1)
drivers/storage/ata.c    polled ATA PIO disk driver (primary IDE)
drivers/storage/ahci.c   polled AHCI driver for SATA disks/SSDs (MMIO + DMA)
drivers/storage/nvme.c   minimal NVMe driver for PCIe SSDs (admin + I/O queues)
drivers/storage/sd.c     SD/MMC card driver over an SDHCI host controller
drivers/storage/scsi.c   SCSI disk layer; adapters in vscsi.c (virtio-scsi) and pvscsi.c (VMware PVSCSI)
drivers/storage/ufs.c    polled UFS driver over a UFSHCI host controller (UPIU transport, SCSI commands)
drivers/storage/atapi.c  polled ATAPI driver for CD/DVD drives (PACKET command)
drivers/net/nic.c        link-layer driver dispatch (igb / e1000 / vmxnet3 / rtl8139)
drivers/net/rtl8139.c    RTL8139 NIC driver (QEMU default)
drivers/net/e1000.c      Intel e1000 NIC driver, 8254x/82574/I217-I219 (MMIO, RX/TX rings, IRQ)
drivers/net/igb.c        Intel igb NIC driver, I210/I211/82576 (advanced descriptors, poll RX)
drivers/net/vmxnet3.c    VMware vmxnet3 NIC driver (rev 1, single queue, poll RX)
drivers/usb/usb.c        UHCI control transfers (lsusb)
drivers/usb/xhci.c       xHCI host + USB mass-storage block device
drivers/input/keyboard.c PS/2 keyboard (IRQ1), input ring, US/FR layouts
drivers/input/mouse.c    PS/2 mouse (IRQ12), shares the i8042 with the keyboard
drivers/video/fb.c       32-bpp framebuffer (DISPI program or bootloader adopt)
drivers/video/mtrr.c     MTRR/PAT write-combining for the framebuffer
drivers/video/edid.c     monitor EDID over DDC/I2C (real-HW)
drivers/audio/sb16.c     Sound Blaster 16 (8-bit PCM over ISA DMA, WAV playback)
drivers/audio/hda.c      Intel HD Audio + Realtek codec (16-bit PCM, WAV playback)
drivers/audio/es1371.c   Ensoniq ES1371/ES1373 AudioPCI (AC'97, 16-bit PCM, WAV playback)
drivers/audio/speaker.c  PC speaker tones (PIT channel 2)
drivers/acpi/power.c     shutdown / reboot + ACPI table walking
drivers/acpi/aml.c       tiny AML interpreter (battery methods)
drivers/acpi/battery.c   best-effort battery percentage (real-HW)

user/lib/          user libc: syscall wrappers, gfx, inflate, png, url, _start
user/curl.c        HTTP/1.1 client
user/browser/      web browser (fetch + HTML + render)
user/imgview.c     PNG image viewer
user/pdfview/      PDF viewer (user/lib/pdf: parser + page renderer)
user/editor.c      windowed text editor (menus, Ctrl+key)
user/edit.c        ed-style line editor over owofs
user/files.c       file-browser app
user/clock.c       clock app
user/about.c       about box
user/crashme.c     fault demo (exercises the crash reporter)

Makefile           build + run + iso + usb
CLAUDE.md          cross-cutting notes (what spans files or isn't obvious)
```

---

## Known simplifications

The kernel has a full low-level foundation, but plenty is deliberately
simplified - these are the honest gaps, roughly in priority order:

- **TLS does not verify the server certificate** (no X.509/RSA/ECDSA), so HTTPS
  resists eavesdropping but not an active MITM. Certificate verification is the
  headline next step.
- **TCP** has no congestion control, no out-of-order reassembly, retransmits
  only the SYN/SYN-ACK, and has no TIME_WAIT - fine over SLIRP, fragile on a
  lossy link.
- **owofs** allocates file data contiguously; the on-disk format is minimal.
- **Filesystems other than FAT** (HFS+, ext) are read-only.
- **`fork`** eagerly copies the whole address space (no copy-on-write); the
  run-image syscall has no environment and a single-page argument block;
  orphaned zombies aren't reaped.
- **The scheduler** is plain round-robin with no priorities or general
  sleep/timeout.
- **USB phone tethering** is the goal but not done - xHCI enumeration and
  mass-storage work; the RNDIS/CDC NIC driver doesn't.
- **UEFI** boots through GRUB, but GRUB 2.14's relocator can page-fault under
  strict-W^X OVMF before our kernel even runs; a native EFI stub is the only
  fully robust UEFI path and isn't written yet.

See `CLAUDE.md` for the cross-cutting design notes - boot ordering, the memory
model, the WM performance rules, the network-stack `sti; hlt` invariant, and the
other things that aren't obvious from a single file.
