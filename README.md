# OwOS

A 32-bit x86 operating system written from scratch in C and a little GAS
assembly — no host libc, no cross-compiler, no existing kernel underneath. QEMU
boots the freestanding kernel directly through its built-in Multiboot loader; a
GRUB ISO is also produced for real VMs and bare metal.

What started as a "does it boot" experiment now has a graphical desktop with a
window manager and mouse, ring-3 processes with `fork`/`exec`/`waitpid`, block
drivers for IDE/SATA/NVMe/SD/USB/CD-DVD, half a dozen filesystems, a from-scratch
TCP/IP + TLS network
stack with a real web browser on top, an SMB2 file server, a BitTorrent client,
USB host support, and Sound Blaster audio. With no framebuffer it falls back to
an 80×25 text console and everything still works.

```
make            # build owos.bin (host gcc -m32, no cross-compiler)
make run        # QEMU: graphical window + COM1 mirrored to the terminal
make debug      # headless: console only, over COM1
make run NIC=e1000   # attach the Intel 82540EM instead of the default RTL8139
```

---

## Build & run

You need `gcc` with 32-bit support (`-m32`) and `qemu-system-i386`. Building the
bootable ISO additionally needs `grub-mkrescue`, `xorriso`, and `mtools`; the
seeded FAT test disks need `mkfs.fat` (dosfstools).

| Target | What it does |
| --- | --- |
| `make` | Build `owos.bin` (a freestanding 32-bit ELF linked at 1 MiB). |
| `make run` | Boot in QEMU with a graphical window; COM1 mirrored to the terminal. Adds Sound Blaster 16, the PC speaker, and a USB tablet. |
| `make debug` | Headless — no window, the console comes out on the terminal over COM1, so a whole session can be driven and captured as text. |
| `make iso` | Wrap the kernel in GRUB and emit a hybrid BIOS+UEFI `owos.iso`. |
| `make run-iso` | Boot the ISO on legacy BIOS (SeaBIOS), CD only, the way a real VM would. |
| `make run-iso-uefi` | Boot the same ISO through UEFI (OVMF firmware). |
| `make usb USB=/dev/sdX` | Flash `owos.iso` to a removable stick (guarded — it refuses anything that isn't a removable whole-disk, and asks for confirmation, because it erases the drive). |
| `make clean` | Remove build artifacts (keeps the disk images). |
| `make distclean` | Also wipe the disk images. |

There is **no cross-compiler**. The kernel is built with the host
`gcc -m32 -ffreestanding -nostdlib` and linked at 1 MiB by `linker.ld`. QEMU's
`-kernel` is itself a Multiboot loader, so nothing else is needed there. The ISO
exists only for the targets that genuinely need a bootloader on the media —
VMware, VirtualBox, bare metal — and carries GRUB for both BIOS and UEFI.

`make run`/`debug` auto-create the disks and devices the drivers exercise: an
IDE disk (`disk.img`, the owofs boot volume), a SATA disk on an AHCI controller,
an NVMe SSD, a FAT-formatted USB stick on an xHCI controller, an SD card on an
SDHCI controller, an ISO 9660 `cd.iso` in an ATAPI CD-ROM, a SLIRP user-mode NIC,
and (for `run`) Sound Blaster 16 + a USB tablet. Files written through the shell
persist in `disk.img` across reboots.

> **Don't pass `-vga std`.** It leaves the card in a graphics mode where the
> 0xB8000 text window is unmapped and nothing renders. `make run` uses QEMU's
> default adapter on purpose so the framebuffer path works.

---

## What's inside

### Boot & firmware

`boot/boot.S` carries **two** Multiboot headers: Multiboot1 (for QEMU `-kernel`,
which hands us the info struct directly) and Multiboot2 (for GRUB on both BIOS
and UEFI, whose mb1 handoff page-faults under UEFI). On the mb2 path the kernel
walks the tag chain and synthesizes an mb1-shaped info struct, so the PMM and
framebuffer code need no per-format logic. Along the way it detects the firmware
type (BIOS vs UEFI), captures the ACPI RSDP, and records the SMBIOS entry point —
all things the legacy BIOS scans can't find under UEFI.

### Graphics & the desktop

A 32-bpp linear framebuffer, a PS/2 mouse, and a compositing window manager.
fb.c gets the framebuffer one of two ways: it programs QEMU's Bochs/DISPI VBE
registers (`-kernel`), or it **adopts** the framebuffer GRUB/real hardware hands
it. The 80×25 VGA console keeps running underneath as a character grid that the
compositor renders with an 8×16 CP437 font, so the shell needs zero changes to
run in a window.

The window manager (`wm.c`) is a single kernel thread: draggable title bars,
raise-on-click z-order, focus, close boxes, a cursor, and a clock/taskbar with a
Start menu. Performance is real on hardware (QEMU hides it) — a cached back
buffer above 16 MiB, damage-rectangle-only presents, and a write-combining front
buffer took a laptop from 2 fps to smooth.

Desktop apps open from the Start menu. Some are kernel-side (the terminal that
hosts the shell, an About box); most are **ring-3 user programs** that draw into
a shared pixel buffer through the windowing syscalls (`uwin.c`): a clock, a file
browser, a text editor, an image viewer, and a real web browser. A ring-3 app
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

- **`fork`** duplicates the caller — address space and trap frame copied, the
  child's `eax` forced to 0.
- **the run-image syscall** (`exec`) replaces the current image with a fresh ELF
  from disk, rewriting the trap frame and swapping CR3 before freeing the old
  address space.
- **`exit`** makes a zombie; the parent reaps it via `waitpid`.

Processes carry a **cwd**, so `curl -o out.txt` lands in the current directory.
Each has a file-descriptor table (0/1/2 = console); `pipe` creates a kernel ring
buffer, fds survive `fork`/`exec`, and the shell wires `a | b` pipelines
kernel-side. User programs are freestanding ELF32 linked at the user window and
embedded into the kernel image; the loader parses the program headers into a
fresh address space.

### Storage & filesystems

Three drivers cover the common disk interfaces — a polled ATA PIO driver (IDE),
a polled AHCI driver (SATA, MMIO + DMA), and a minimal NVMe driver (PCIe SSD,
admin/I/O queues). `blk.c` picks **one** of them at boot (first present of IDE →
AHCI → NVMe) to back **owofs**, a flat 512-byte-sector filesystem with a
directory tree that persists across reboots. Two safety rules matter on real
hardware: owofs auto-formats **only a blank IDE disk** (never the user's
SATA/NVMe OS drive), and if there's no usable disk it falls back to a volatile
RAM disk so there's always a working filesystem.

Removable volumes mount read/write or read-only at **`/media`** via `mount` /
`eject`:

- **FAT12/16/32** — full read **and** write (host `fsck`-clean).
- **HFS+** ("Mac OS Extended") — read-only.
- **ext2/3/4** — read-only (extent trees + indirect blocks).
- **ISO 9660 + Joliet** — read-only CD/DVD (long, mixed-case names).

USB flash drives are handled by the xHCI mass-storage path (BOT/SCSI) and SD/MMC
cards by an SDHCI driver, so a plugged-in stick or card shows up as a block device
and mounts the same way. A **CD/DVD** mounts too: a polled **ATAPI** driver tunnels
SCSI commands over the IDE bus (2048-byte sectors) and the ISO 9660 driver reads
the disc.

`storage` lists every detected device with model and capacity; `neofetch` shows
them too.

### Networking

A from-scratch stack, strictly layered:

- **Link layer** — a NIC dispatch layer (`nic.c`) probes the RTL8139 (QEMU's
  default) and the Intel e1000 (8254x/82574/I21x; `make run NIC=e1000`) and
  keeps whichever is present. The rest of the stack is driver-agnostic.
- **IPv4** (`net.c`) — ARP, ICMP echo, UDP, DNS, and a DHCP client.
- **TCP** (`tcp.c`) — active **and** passive open, several concurrent
  connections, SYN-accepted-in-IRQ with a daemon thread taking the connection.
- **TLS** (`tls.c` + `crypto.c`) — a client-only TLS 1.3 (1.2 fallback) with
  X25519, AES-128-GCM, and SHA-256/HMAC/HKDF, freestanding and allocation-free.
  The server certificate is **not** verified yet — deliberate: it resists
  passive eavesdropping, not an active MITM.

On top of that:

- `ping <host>`, `dig <host> [type]` (A/AAAA/CNAME/MX/TXT/NS…), `ntp` to set the
  clock, `dhcp` to re-request a lease.
- **`curl <url>`** — a ring-3 HTTP/1.1 client (the only user-facing network API
  is four `net_sock_*` syscalls): `http://` or `https://`, `-i`, `-X`, `-o`.
- **A graphical web browser** — a ring-3 desktop app that fetches over HTTP(S),
  inflates gzip/deflate, parses HTML, decodes PNG images, and renders the page,
  streaming downloads to `~/Downloads`.

### Services & the SMB server

A small service manager (`services.c`) runs headless background daemons from an
enum-indexed registry: a 1 Hz `beat`, a UDP echo server (`echod`, RFC 862), a
UDP daytime server (`daytimed`, RFC 867), a TCP echo server (`echotcp`), and
**`smbd`** — a read-only **SMB2 file server** that shares owofs to Linux
`smbclient` / `mount -t cifs` and Windows/KDE clients (verified byte-exact on
>64 KiB files). UDP daemons serve straight from the receive interrupt; TCP
daemons own an accept-loop thread. `make run`/`debug` forward host port 1445 to
the guest's 445 so `smbd` is reachable.

### BitTorrent client

A from-scratch BitTorrent stack: a bencode parser (`bencode.c`), `.torrent`
metainfo parsing with the SHA-1 infohash (`torrent.c`), tracker announce
(`tracker.c`), and the peer wire protocol (`peer.c`) — handshake, piece
download, and SHA-1 verification. `torrent get <file>` downloads a whole
single-file torrent to disk.

### Audio

The PC speaker plays square-wave tones (`beep`), and a Sound Blaster 16 driver
(8-bit PCM over ISA DMA) plays real sound: `play <tune>` for a built-in melody
(twinkle / megalovania / sweden) or `play <file.wav>` to stream a RIFF/WAVE file
from disk (any PCM 8/16-bit mono/stereo is down-converted; Ctrl+C stops it).

### USB

`lsusb` walks the USB host controllers — for UHCI (QEMU's PIIX3) it drives a
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
`neofetch` shows it all next to an OwO logo — including a "Host:" line read from
SMBIOS/DMI and (on real Intel graphics) the monitor refresh rate over DDC/EDID.
Print Screen captures the whole framebuffer to a PNG in your home directory. The
shell keeps a command history (Up/Down) and supports in-line editing. The
keyboard layout switches at runtime (`loadkeys us|fr`), with US QWERTY and French
AZERTY (including AltGr) built in.

---

## Shell commands

```
help  clear  uptime  sleep  meminfo  memtest  tasks
ls  pwd  cd  cat  grep  write  cp  mv  rm  mkdir  rmdir
zip  unzip  gzip  gunzip
run  format  which  echo
storage  mount  eject  lspci  lsusb
ping  dig  ntp  curl  url  torrent
dhcp  ethdiag  txtest            (NIC bring-up / diagnostics)
services                          (list/start/stop/restart/status)
beep  play  cpuinfo  uname  whoami  loadkeys  date  neofetch
poweroff  restart
```

Any command's output can be piped into `grep` (`<cmd> | grep <pattern>`) or
redirected to a file (`<cmd> > <file>`). A bare non-builtin name runs the
matching `/bin` program, so `curl <url>`, `editor file.txt`, and friends just
work. Bundled ring-3 programs: `edit`, `editor`, `curl`, `browser`, `imgview`,
`files`, `clock`, `about`, `crashme`.

---

## Per-file layout

```
boot/boot.S        Multiboot1+2 headers, entry point, stack setup

kernel/kernel.c    kernel_main — brings subsystems up in dependency order
kernel/gdt.c .S    flat 32-bit GDT + lgdt/segment reload
kernel/idt.c isr.S CPU exception IDT + dispatcher (panic screen + COM1)
kernel/irq.c .S    PIC remap + hardware IRQ dispatch
kernel/timer.c     PIT (250 Hz), tick counter + sleep (IRQ0)
kernel/serial.c    polled COM1 UART (console mirror / logging)
kernel/console.c   text + framebuffer console funnel (was vga.c)
kernel/shell.c     line-buffered shell: history, editing, pipes, redirection
kernel/crash.c     ring-3 crash reporter (fault -> crash window)

kernel/pmm.c       physical frame allocator (bitmap over the mmap)
kernel/paging.c    identity map, frame pool, per-process address spaces
kernel/heap.c      kmalloc/kfree (first-fit free list)
kernel/kstring.c   freestanding memory primitives
kernel/sched.c .S  scheduler: kernel threads + user processes, fork, reaping
kernel/syscall.c   int 0x80 dispatch (write..net..win)
kernel/loader.c    ELF32 loader: load segments into a fresh address space
kernel/pipe.c      kernel pipe ring buffers
kernel/file.h      per-process fd table

kernel/wm.c        compositing window manager (one kernel thread)
kernel/uwin.c      ring-3 windowing server (user apps own a window)
kernel/apps.c      desktop app registry / Start menu
kernel/screenshot.c Print Screen -> PNG capture worker
kernel/png.c       PNG decoder (over the DEFLATE engine)

kernel/blk.c       block-device selection for owofs (IDE>AHCI>NVMe) + /media mount
kernel/fs.c        owofs: minimal persistent filesystem
kernel/fat.c       FAT12/16/32 read+write driver (mounts at /media)
kernel/hfs.c       HFS+ read-only driver
kernel/ext.c       ext2/3/4 read-only driver
kernel/iso9660.c   ISO 9660 + Joliet read-only CD/DVD driver

kernel/net.c       IPv4: ARP, ICMP, UDP/DNS, DHCP
kernel/tcp.c       concurrent TCP: client (connect) + server (listen/accept)
kernel/tls.c       client TLS 1.3/1.2 (for https)
kernel/crypto.c    SHA-256/HMAC/HKDF, AES-128-GCM, X25519
kernel/url.c       freestanding URL parser/builder
kernel/services.c  service manager: supervisor + headless daemons
kernel/smb.c       read-only SMB2 file server (smbd)
kernel/bencode.c   bencode parser
kernel/torrent.c   .torrent metainfo + SHA-1 infohash
kernel/tracker.c   BitTorrent tracker announce
kernel/peer.c      BitTorrent peer wire protocol

kernel/zip.c       LZSS compressor/decompressor (zip/unzip)
kernel/gzip.c      gzip/DEFLATE codec + CRC32 (gzip/gunzip, curl, png)
kernel/smbios.c    SMBIOS/DMI host name for neofetch
kernel/firmware.c  BIOS vs UEFI detection
kernel/cpu.c       CPUID vendor/brand
kernel/rtc.c       CMOS real-time clock
kernel/neofetch.c  system-info splash

drivers/ — device drivers, grouped by type (each subdir is on the -I path,
          so cross-file includes stay flat: #include "fb.h", not a path):

drivers/bus/pci.c        PCI bus enumeration (config mechanism #1)
drivers/storage/ata.c    polled ATA PIO disk driver (primary IDE)
drivers/storage/ahci.c   polled AHCI driver for SATA disks/SSDs (MMIO + DMA)
drivers/storage/nvme.c   minimal NVMe driver for PCIe SSDs (admin + I/O queues)
drivers/storage/sd.c     SD/MMC card driver over an SDHCI host controller
drivers/storage/atapi.c  polled ATAPI driver for CD/DVD drives (PACKET command)
drivers/net/nic.c        link-layer driver dispatch (rtl8139 / e1000)
drivers/net/rtl8139.c    RTL8139 NIC driver (QEMU default)
drivers/net/e1000.c      Intel e1000 NIC driver (MMIO, RX/TX rings, IRQ)
drivers/usb/usb.c        UHCI control transfers (lsusb)
drivers/usb/xhci.c       xHCI host + USB mass-storage block device
drivers/input/keyboard.c PS/2 keyboard (IRQ1), input ring, US/FR layouts
drivers/input/mouse.c    PS/2 mouse (IRQ12), shares the i8042 with the keyboard
drivers/video/fb.c       32-bpp framebuffer (DISPI program or bootloader adopt)
drivers/video/mtrr.c     MTRR/PAT write-combining for the framebuffer
drivers/video/edid.c     monitor EDID over DDC/I2C (real-HW)
drivers/audio/sb16.c     Sound Blaster 16 (8-bit PCM over ISA DMA, WAV playback)
drivers/audio/hda.c      Intel HD Audio + Realtek codec (16-bit PCM, WAV playback)
drivers/audio/speaker.c  PC speaker tones (PIT channel 2)
drivers/acpi/power.c     shutdown / reboot + ACPI table walking
drivers/acpi/aml.c       tiny AML interpreter (battery methods)
drivers/acpi/battery.c   best-effort battery percentage (real-HW)

user/lib/          user libc: syscall wrappers, gfx, inflate, png, url, _start
user/curl.c        ring-3 HTTP/1.1 client
user/browser/      ring-3 web browser (fetch + HTML + render)
user/imgview.c     ring-3 PNG image viewer
user/editor.c      ring-3 windowed text editor (menus, Ctrl+key)
user/edit.c        ring-3 ed-style line editor over owofs
user/files.c       ring-3 file-browser app
user/clock.c       ring-3 clock app
user/about.c       ring-3 about box
user/crashme.c     ring-3 fault demo (exercises the crash reporter)

linker.ld          loads the kernel at 1 MiB, multiboot header first
Makefile           build + run + iso + usb
CLAUDE.md          cross-cutting notes (what spans files or isn't obvious)
```

---

## Known simplifications

The kernel has a full low-level foundation, but plenty is deliberately
simplified — these are the honest gaps, roughly in priority order:

- **TLS does not verify the server certificate** (no X.509/RSA/ECDSA), so HTTPS
  resists eavesdropping but not an active MITM. Certificate verification is the
  headline next step.
- **TCP** has no congestion control, no out-of-order reassembly, retransmits
  only the SYN/SYN-ACK, and has no TIME_WAIT — fine over SLIRP, fragile on a
  lossy link.
- **owofs** allocates file data contiguously; the on-disk format is minimal.
- **Filesystems other than FAT** (HFS+, ext) are read-only.
- **`fork`** eagerly copies the whole address space (no copy-on-write); the
  run-image syscall has no environment and a single-page argument block;
  orphaned zombies aren't reaped.
- **The scheduler** is plain round-robin with no priorities or general
  sleep/timeout.
- **USB phone tethering** is the goal but not done — xHCI enumeration and
  mass-storage work; the RNDIS/CDC NIC driver doesn't.
- **UEFI** boots through GRUB, but GRUB 2.14's relocator can page-fault under
  strict-W^X OVMF before our kernel even runs; a native EFI stub is the only
  fully robust UEFI path and isn't written yet.

See `CLAUDE.md` for the cross-cutting design notes — boot ordering, the memory
model, the WM performance rules, the network-stack `sti; hlt` invariant, and the
other things that aren't obvious from a single file.
