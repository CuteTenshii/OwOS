# OwOS

A small x86 operating system, built from scratch.

## Current state

The base boots. OwOS is a 32-bit Multiboot kernel that QEMU loads directly
(no GRUB, no ISO needed for QEMU; a GRUB ISO is available for real VMs/bare
metal). On boot it:

1. Enters via `_start` (boot/boot.S) in 32-bit protected mode, masks IRQs,
   and sets up a stack.
2. Calls `kernel_main` (kernel/kernel.c).
3. Installs its own flat GDT (kernel/gdt.c), replacing the bootloader's.
4. Installs an IDT with handlers for the 32 CPU exceptions (kernel/idt.c).
5. Remaps the PIC and installs the 16 hardware IRQ handlers (kernel/irq.c).
6. Reads the multiboot memory map and tracks physical frames (kernel/pmm.c),
   identity-maps the low 16 MiB and enables paging (kernel/paging.c), and sets
   up a kernel heap (kernel/heap.c).
7. Enumerates the PCI bus (kernel/pci.c).
8. Brings up the RTL8139 NIC and the IPv4 stack (kernel/net.c), if a card is
   present.
9. Mounts the ATA disk and the owofs filesystem (kernel/fs.c), formatting it
   if blank.
10. Sets up user mode (kernel/syscall.c, kernel/loader.c): the int 0x80 gate,
    the ring-3 memory window, and installs the embedded programs to disk.
11. Enables the timer (IRQ0) and keyboard (IRQ1), probes the Sound Blaster 16
    (IRQ5), starts the preemptive scheduler with a couple of worker threads
    (kernel/sched.c), brings up the service supervisor and auto-starts its
    headless daemons (kernel/services.c), turns on interrupts, and drops into a
    line-buffered shell (kernel/shell.c) running as the "main" task.

A CPU exception (e.g. a fault) is caught and reported — vector, error code,
and faulting EIP — instead of triple-faulting the machine. Hardware
interrupts are remapped to vectors 32-47 and enabled; the timer keeps time
in the background. The keyboard handler buffers keystrokes (Shift, Backspace, and the numeric
keypad with a toggleable Num Lock) into a ring; the main loop drains it into
the shell. The active layout is switchable at runtime (`loadkeys us|fr`),
with US QWERTY and French AZERTY built in; AZERTY also handles AltGr (the
third level: `|`, `@`, `#`, `{`, `[`, `]`, `}`, `\`, ...). The PIT is
programmed to 100 Hz, so uptime is accurate and `timer_sleep()` blocks for a
real duration. Everything printed to the console is also mirrored to COM1.

Memory: the multiboot memory map is parsed into a bitmap frame allocator;
paging is on with the low 16 MiB identity-mapped; a first-fit `kmalloc`/`kfree`
heap runs in [4 MiB, 16 MiB). Page faults report the faulting address (CR2).

Multitasking: a preemptive round-robin scheduler switches tasks on every
timer tick. Tasks are kernel threads (ring 0, shared address space) or user
processes (ring 3, private address space + CR3 switched per task). The shell
runs as the "main" task alongside worker threads and any user processes.

Storage: three drivers cover the common disk interfaces. A polled ATA PIO
driver (kernel/ata.c) talks to the primary IDE disk and backs owofs (a minimal
superblock + a 16-entry directory whose slots form a Unix-like tree via parent
pointers, with contiguous file data) that persists across
reboots. Alongside it, a polled AHCI driver (kernel/ahci.c) drives SATA disks
(and SATA SSDs) over MMIO + DMA, and a minimal NVMe driver (kernel/nvme.c)
brings up a PCIe SSD via admin/I/O queues. The `storage` command lists every
detected device with its model and capacity; `neofetch` shows them too.

Buses: PCI is enumerated via the legacy 0xCF8/0xCFC config ports; `lspci`
lists every device with vendor/device IDs and a decoded class. `lsusb` walks
the USB host controllers and probes their root-hub ports.

Networking: an RTL8139 NIC driver (kernel/rtl8139.c) fields the card's
interrupt and moves Ethernet frames; on top of it is a from-scratch IPv4
stack (kernel/net.c) with ARP, ICMP, and UDP, and a single-connection TCP
client (kernel/tcp.c). That's enough to:

- `ping <host>` — resolve a name and ICMP-echo it.
- `dig <host> [type]` — DNS lookups over UDP (A, AAAA, CNAME, MX, TXT, NS, ...).
- `curl <url>` — fetch an `http://` *or* `https://` URL (`-i` to include
  headers, `-X` to set the method, `-o` to save the body to a file). It sends
  `Accept-Encoding: gzip` and transparently inflates a `Content-Encoding: gzip`
  response (kernel/gzip.c).

HTTPS rides a minimal TLS client (kernel/tls.c, kernel/crypto.c): TLS 1.3
(with a 1.2 fallback), `TLS_AES_128_GCM_SHA256` / `…ECDHE_RSA…`, X25519 key
exchange, SHA-256/HMAC/HKDF key schedule, and AES-128-GCM record protection —
all freestanding, no allocation. (The server certificate is **not** verified
yet, so this stops passive eavesdropping but not an active MITM; X.509 + RSA/
ECDSA verification is the documented next step. `cryptotest` self-checks the
primitives against known-answer vectors.)

Services: a small service manager (kernel/services.c) runs headless background
daemons that need no shell to live. A single "servicesd" supervisor thread
ticks every running service; network daemons additionally field traffic from
the NIC's receive interrupt via UDP server hooks in the IPv4 stack
(`net_udp_listen`/`net_udp_send`). Services flagged auto-start come up at boot,
so they're serving before anyone logs in; the `services` command manages them
afterwards:

- `services list` — the registry, each daemon's port and running state.
- `services start|stop|restart <name>` — bring a daemon up or down.
- `services status [name]` — uptime and activity (datagrams served / beats).

Three ship in the box: `beat` (an always-on 1 Hz heartbeat, auto-started),
`echod` (a UDP echo server, RFC 862, port 7), and `daytimed` (a UDP daytime
server, RFC 867, port 13, answering with the RTC clock). The same shape — bind
a port in `on_start`, serve from the receive path, release it in `on_stop` — is
how a fuller SMB/SSH-style server would slot in.

Audio: the PC speaker (PIT channel 2 + port 0x61) plays square-wave tones, and
a Sound Blaster 16 driver (kernel/sb16.c, 8-bit PCM over ISA DMA) plays real
sound. `beep` for a single tone, `play <tune>` for a built-in tune
(twinkle/megalovania/sweden), and `play <file.wav>` to stream a RIFF/WAVE file
from disk (any PCM 8/16-bit mono/stereo is down-converted to 8-bit mono;
Ctrl+C stops it).

Power: `poweroff` shuts the machine down (ACPI/QEMU) and `restart` reboots it
(8042 pulse, falling back to a triple fault).

User mode: programs run in ring 3 as **first-class scheduled processes**, each
in its own address space. A process gets a fresh page directory that shares
the kernel/heap mappings but has a private user window [2 MiB, 4 MiB) backed
by frames from a dedicated page pool ([12 MiB, 16 MiB)). The scheduler treats
processes like any other task, switching CR3 and the TSS kernel stack when it
runs one, so several processes run concurrently with the shell and kernel
threads. The full fork/exec/wait lifecycle works:

- `fork` duplicates the caller — copying its address space and kernel trap
  frame so the child returns 0 while the parent gets the child's pid.
- `exec` replaces the current process's image with a new ELF loaded from disk
  (new address space, rewritten trap frame), keeping the same pid.
- `exit` makes a process a zombie carrying its status; `waitpid` blocks the
  parent until a child exits, returns the status, and reaps it (frames, page
  directory, kernel stack freed).

Programs receive command-line arguments: the shell tokenizes `run prog a b c`
and the kernel lays argc/argv (and the strings) onto the new process's user
stack in the System V layout, so `main(argc, argv)` works.

The shell runs programs in the foreground: it waits for the process and prints
its exit status before showing the prompt again. Programs talk to the kernel
only through `int 0x80` syscalls (write, read, fork, exec, waitpid, exit,
pipe, close, uptime, readfile, writefile); user pointers are bounds-checked, `read`/
`waitpid` block while the scheduler keeps running, and the MMU blocks any
user access to kernel or heap memory.

File descriptors: each process has an fd table (0/1/2 = console). `read`/
`write` act on an fd, `pipe` creates a kernel ring buffer with a read end and
a write end, and `close` drops a reference (a pipe reports EOF once all write
ends are closed). fds are inherited across `fork` and survive `exec`, so a
process can set up a pipe, fork, and stream data between the two ends -- a
blocked end yields the CPU so the other side runs.

The shell builds pipelines: `run a | b` spawns both programs with a's stdout
and b's stdin wired to a kernel pipe (set up before either runs), then waits
for both -- e.g. `run produce | upper` streams produce's output through the
pipe and prints it upper-cased. It also pipes any built-in command's output
into `grep` (`<cmd> | grep <pattern>`) and redirects output into a file
(`<cmd> > <file>`).

User programs are written in C against a tiny user library (user/lib): it
provides the syscall wrappers plus `puts`, `print_uint`, and an echoing
`readline`, and a `_start` that calls `main` and exits with its result. They
are compiled to standard **ELF32 executables**; the kernel's loader
(kernel/elf.h, kernel/loader.c) parses the program headers, loads each
PT_LOAD segment into private frames, and zeroes `.bss`. The bundled `hello`
(user/hello.c) forks a child that `exec`s the `worker` program (user/worker.c)
and waits for it — the classic fork/exec/wait, with `worker`'s exit status
reported back through `waitpid`.

Niceties: CPUID identifies the processor, the CMOS RTC gives the wall-clock
date, and `neofetch` shows it all next to an OwO logo. The shell keeps a
command history (Up/Down) and supports in-line editing (Left/Right/Home/End,
insert/delete).

Shell commands: `help`, `clear`, `uptime`, `sleep <secs>`, `meminfo`,
`memtest`, `tasks`, `ls [path]`, `pwd`, `cd [dir]`, `cp <src> <dst>`,
`mv <src> <dst>`, `rm <file>...`, `mkdir <dir>...`, `rmdir <dir>...`,
`cat <file>`, `grep <p> [f]`, `write <file> <text>`,
`zip <s> [d]`, `unzip <s> [d]`, `gzip <s> [d]`, `gunzip <s> [d]`,
`format`, `lspci`, `lsusb`, `beep`,
`play [tune|file.wav]`, `cpuinfo`,
`uname [-a]`, `whoami`, `date`, `neofetch`, `ping <host>`, `dig <host> [type]`,
`curl <url>`, `cryptotest`, `run <file> [a|b]`, `echo <text>`,
`loadkeys <us|fr>`, `poweroff`, `restart`. Output of any command can be piped
into `grep` (`<cmd> | grep <p>`) or redirected to a file (`<cmd> > <f>`).

## Build & run

Requires `gcc` with 32-bit support (`-m32`) and `qemu-system-i386`. Building
the ISO additionally needs `grub-mkrescue` + `xorriso` (and `mtools`).

```sh
make         # build owos.bin
make run     # boot it in QEMU (window + COM1 on the terminal)
make debug   # headless: no window, console comes out on the terminal via COM1
make iso     # wrap the kernel in GRUB and emit a bootable owos.iso
make run-iso # boot the ISO the way a real VM would (CD only, no -kernel/-hda)
make clean   # remove build artifacts (keeps disk.img)
make distclean  # also wipe disk.img
```

`run`/`debug` auto-create a 16 MiB `disk.img` and attach it as the primary
IDE disk; files written through the shell persist there across reboots. They
also attach an RTL8139 NIC on QEMU's user-mode (SLIRP) network, so `ping`,
`dig`, and `curl` work out of the box. The console is mirrored to the serial
port (COM1), so `make debug` lets you drive and capture a full session as
plain text.

`make run` additionally wires up a Sound Blaster 16 and the PC speaker to your
host audio (PipeWire by default) and adds a USB tablet, so `play` and `lsusb`
have something to talk to. Swap the `audiodev` backend (`alsa`/`sdl`/`pa`/
`coreaudio`) to suit your system, or use `-audiodev wav,id=…,path=out.wav` to
record sound to a file.

QEMU loads `owos.bin` via its built-in Multiboot loader (`-kernel`), so no
bootloader is needed there. `owos.iso` exists for everything that *does* need
one — VMware, VirtualBox, bare metal — by wrapping the same kernel in GRUB.

> Note: `make run` uses QEMU's default VGA adapter on purpose. Passing
> `-vga std` leaves the card in a graphics mode where the 0xB8000 text
> window is unmapped and nothing shows.

## Layout

```
boot/boot.S      multiboot header + entry point
kernel/kernel.c  kernel_main
kernel/gdt.c     flat 32-bit GDT
kernel/gdt_flush.S  lgdt + segment-register reload
kernel/idt.c     IDT + CPU exception dispatcher
kernel/isr.S     exception stubs (vectors 0-31)
kernel/irq.c     PIC remap + hardware IRQ dispatch
kernel/irq_stubs.S  IRQ stubs (vectors 32-47)
kernel/keyboard.c   PS/2 keyboard driver (IRQ1) + input ring buffer
kernel/timer.c   PIT @ 100 Hz, tick counter + sleep (IRQ0)
kernel/serial.c  polled COM1 UART (console mirror / logging)
kernel/shell.c   line-buffered command shell (history, editing, pipes)
kernel/pmm.c     physical frame allocator (bitmap over the mmap)
kernel/paging.c  identity-map, frame pool, per-process address spaces
kernel/heap.c    kmalloc/kfree (first-fit free list)
kernel/kstring.c freestanding memory primitives (kmemcpy)
kernel/sched.c   scheduler: kernel threads + user processes, fork, reaping
kernel/services.c service manager: supervisor + headless UDP/TCP daemons (echod, daytimed, echotcp, smbd)
kernel/smb.c     read-only SMB2 file server (smbd): shares owofs to Linux/Windows clients
kernel/ata.c     polled ATA PIO disk driver (primary IDE)
kernel/ahci.c    polled AHCI driver for SATA disks/SSDs (MMIO + DMA)
kernel/nvme.c    minimal NVMe driver for PCIe SSDs (admin + I/O queues)
kernel/fs.c      owofs: minimal persistent filesystem
kernel/zip.c     LZSS compressor/decompressor (zip/unzip commands)
kernel/gzip.c    gzip codec: DEFLATE inflate + dynamic/fixed/stored deflate + CRC32 (gzip/gunzip, curl)
kernel/pci.c     PCI bus enumeration (config mechanism #1)
kernel/usb.c     USB host-controller listing (lsusb)
kernel/rtl8139.c RTL8139 NIC driver (Ethernet link layer)
kernel/net.c     IPv4 stack: ARP, ICMP, UDP/DNS, ping/dig/curl
kernel/tcp.c     concurrent TCP: client (connect) + server (listen/accept)
kernel/tls.c     minimal TLS 1.3/1.2 client (for https curl)
kernel/crypto.c  SHA-256/HMAC/HKDF, AES-128-GCM, X25519
kernel/speaker.c PC speaker tones (PIT channel 2)
kernel/sb16.c    Sound Blaster 16 driver (8-bit PCM over ISA DMA, WAV playback)
kernel/cpu.c     CPUID vendor/brand
kernel/rtc.c     CMOS real-time clock
kernel/neofetch.c  system-info splash
kernel/power.c   shutdown / reboot
kernel/syscall.c int 0x80 syscall dispatch
kernel/loader.c  ELF32 loader: load segments + run a ring-3 program
kernel/elf.h     ELF32 header/program-header structs
kernel/file.h    per-process file descriptor table
kernel/pipe.c    kernel pipe ring buffers
kernel/switch.S  context switch, thread + user-task + fork-child trampolines
user/hello.c     ring-3 program: fork + exec + waitpid demo
user/worker.c    ring-3 program that hello exec's into
user/args.c      ring-3 program that prints its argv
user/pipe.c      ring-3 program: pipe + fork demo
user/produce.c   pipeline stage: writes lines to stdout
user/upper.c     pipeline stage: upper-cases stdin to stdout
user/edit.c      ring-3 line editor (ed-style) over owofs files
user/curl.c      ring-3 HTTP/1.1 client: URL parse + request/response framing over the net_sock_* syscalls
user/lib/        user-space library (syscall wrappers, puts, readline, _start)
user/user.ld     flat link script for user programs (entry at 0x200000)
kernel/multiboot.h  multiboot info / memory-map structs
kernel/io.h      port I/O (inb/outb)
kernel/vga.c     VGA text-mode console driver (mirrors to serial)
linker.ld        loads the kernel at 1 MiB, multiboot header first
Makefile         build + run + iso
```

## Next

The kernel now has a full low-level foundation: GDT, IDT, PIC, timer,
keyboard, serial, a shell, memory management (physical frames, paging, a
heap), a preemptive scheduler running kernel threads and concurrent ring-3 user
processes (with `fork`), persistent storage (ATA + a simple filesystem), PCI
and USB enumeration, an IPv4/TCP/TLS network stack, and Sound Blaster audio.

Known simplifications to revisit:
- Paging only identity-maps the low 16 MiB; there's no on-demand mapping of
  PMM frames into fresh virtual addresses yet, so the heap is capped at that
  window. A `paging_map(virt, phys)` (allocating page tables from the PMM)
  would lift this.
- The heap free list coalesces forward only.
- The scheduler is plain round-robin with no priorities. `waitpid` blocks on
  a specific pid (or -1 for any) but there's no general sleep/timeout.
- owofs allocates file data contiguously by bumping a pointer; rewriting or
  removing a file leaks its sectors, and the ATA driver polls (no IRQ14).
- `fork` eagerly copies the whole address space (no copy-on-write). The
  `exec` syscall passes only argv[0] (the shell passes full argv); there's no
  environment, and the argument block must fit in one page. Orphaned zombies
  (parent exits without waiting) are not reaped. Syscalls take raw user
  pointers (bounds-checked, no copy-in/out).
- File descriptors cover the console and pipes only (no `open` of disk files;
  no user-space `dup2` -- the shell wires pipelines kernel-side). Blocking ends
  busy-yield rather than sleeping on a wait queue, and a pipeline is limited to
  two stages (`a | b`, not `a | b | c`).
- The frame pool is a simple bump+freelist allocator over a fixed 4 MiB
  window; no demand paging or swapping.
- The network stack is a client only: TCP is one connection at a time with no
  retransmission beyond resending the SYN, no out-of-order reassembly, and no
  congestion control; TLS does **not** verify the server certificate (no
  X.509/RSA/ECDSA), so it resists eavesdropping but not an active MITM.

Natural next steps: X.509 + signature verification to make TLS trustworthy;
multi-stage pipelines + `open`/redirection of disk files for user programs,
plus a user-space `dup2`, to round out the fd layer; an environment + full
argv for the `exec` syscall; copy-on-write fork; a general sleep/timeout; or a
TCP *server* path and a listening socket now that the link layer and IPv4 are
in place.
