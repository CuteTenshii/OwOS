# CLAUDE.md

OwOS is an x86 Unix-like hobby kernel written from scratch in C + GAS
assembly, booted by QEMU's Multiboot loader. One source tree builds both 32-bit
(`i386`, the default and primary target) and 64-bit (`make ARCH=x86_64`); these
notes assume the 32-bit build unless stated. README.md has the feature list and
per-file layout; this file covers only what spans multiple files or isn't
obvious from one read.

**Source layout:** drivers live under `drivers/<type>/`
(`storage net audio usb input video acpi bus`); the core kernel, filesystems
(`fat/hfs/ext/iso9660/ntfs`), block dispatch (`blk`), and the network stack
(`net/tcp/tls/crypto`) stay in `kernel/`. User space is split:
`user/lib/<name>/` holds reusable libraries (libc, gfx, js, media, webp, json,
notify, curl, crypto, sqlite, …); programs live in `user/apps/gui/` (windowed)
and `user/apps/cli/` (terminal tools), each embedded in the kernel as a blob.

- Includes are **flat** (`#include "fb.h"`, never a path): the build puts
  `kernel/` and every `drivers/` subdir on `-I` (`DRVDIRS`, in toolchain.mk).
- Moving a driver between folders is an OBJS path change only — no include edits.
- **Header basenames must stay unique.**
- **The build is split by concern:** the root `Makefile` `include`s
  `toolchain.mk` (flags + arch select), `user/lib/build.mk`, `user/apps/build.mk`,
  `kernel/build.mk` (OBJS + link), and `run.mk` (QEMU targets) — one
  non-recursive graph, all paths root-relative.

## Build & run

```sh
make            # build owos.bin (32-bit, host gcc -m32, no cross-compiler)
make ARCH=x86_64    # build owos-x86_64.bin (64-bit long mode)
make run        # QEMU window + COM1 mirrored to the terminal
make debug      # headless: console on the terminal via COM1
make iso        # wrap in GRUB -> hybrid BIOS+UEFI owos.iso (real VMs/HW)
make run-iso    # boot owos.iso on LEGACY BIOS (SeaBIOS, CD only)
make run-iso-uefi   # boot the same ISO through UEFI (OVMF)
make usb USB=/dev/sdX   # flash owos.iso to a removable stick (guarded; erases it)
make clean      # keeps disk.img;  make distclean  also wipes it
make run NIC=e1000  # attach the Intel 82540EM instead of the default RTL8139
make run SMP=8      # expose 8 cores to the guest (default 4; SMP=1 = uniprocessor)
make run ACCEL=tcg  # force pure emulation (default uses KVM/HVF, ~10-50x faster)
```

- No cross-compiler on Linux: freestanding ELF (`gcc -m32`/`-m64
  -ffreestanding -nostdlib`) linked at 1 MiB by `linker.ld`/`linker64.ld`, loaded
  via QEMU `-kernel`. **User programs are always 32-bit** even under an x64 kernel
  (the kernel embeds each as an opaque blob), so user code uses the i686 toolchain.
- `make run`/`debug` auto-create every disk a storage driver exercises (IDE
  `disk.img`, AHCI, NVMe, USB stick, SD card, ISO 9660 `cd.iso` on ATAPI CD),
  a SLIRP NIC, and a USB tablet; `make run` also adds SB16 + PC speaker.
- All of rtl8139/e1000/igb are built in; the kernel keeps whichever QEMU attached.
- `make iso` is the only target needing a host bootloader (`grub-mkrescue`).
- Diagnostics gate the build — fix compile/link errors first.
- clang editor diagnostics analyze in 64-bit mode and flag every pointer/int
  cast: **false positives** under the real `-m32` build; ignore them.

## Verifying changes

No test framework. The kernel is verified by **booting in QEMU and observing
real behavior** over the monitor — "it compiled" is not "it works", and a bad
change usually triple-faults (silent reboot loop) rather than erroring. The
console mirrors to COM1, so capture a session as text:

```sh
{ sleep 1.5
  for c in r u n spc h e l l o; do printf "sendkey $c\n"; sleep 0.05; done
  printf 'sendkey ret\n'; sleep 2
  printf 'quit\n'
} | timeout 16 qemu-system-i386 -kernel owos.bin -hda disk.img \
      -display none -serial file:/tmp/s.log -monitor stdio >/dev/null 2>&1
cat -v /tmp/s.log
```

- `sendkey` quirks (else fake "bugs"): letters are lowercase key names
  (`sendkey a`), a capital is `shift-a`, `.` is `dot`, space is `spc`, `|` is
  `shift-backslash`. Always send those by name.
- **macOS:** `timeout` is GNU-only — use `gtimeout` (`brew install coreutils`), or
  run QEMU in the background and `kill` it after a `sleep`. The build needs an
  ELF cross-toolchain — `i686-elf-` for the default build, `x86_64-elf-` for
  `ARCH=x86_64` (toolchain.mk auto-selects per ARCH on Darwin); see README.
- A reboot loop is a triple fault: rerun with `-no-reboot -d int,cpu_reset`.
- On a fault, idt.c panics (vector, error code, EIP, CR2 for #PF) to COM1 and as
  a heapless framebuffer screen; the error-code present/write/**user** bits say
  whether it came from ring 3.
- `info registers` / `xp` over `-monitor stdio` read CPU state and physical mem.
- Do **not** pass `-vga std`: it unmaps the 0xB8000 text window and nothing
  renders. The default adapter keeps SeaBIOS's text mode 3.
- The battery and xHCI/tethering paths are **real-hardware-only**; keep their
  diagnostic plumbing (`battery_error`, `aml_last_error`, per-port xHCI lines).
- Remove any deliberate-fault test you add, and re-verify a clean boot.

## Hard rules

These change how you edit; they are the rules most likely to be violated.

- **`exec` + `(` is blocked by a repo hook** (misfires as a Node child-process
  call). So the kernel fn is `sched_replace`, the libc wrapper is `execp`, and
  comments say "the run-image syscall" — never spell it with a parenthesis.
- **`sti` before `hlt` in every thread-context wait loop.** `context_switch`
  doesn't save/restore EFLAGS, so a thread resumed by a cooperative
  `sched_yield` from an IF-clear context inherits IF=0; a bare `hlt` then halts
  the CPU forever. The `int 0x80` gate also runs IF=0, so anything a syscall
  reaches that spins on `hlt` must `sti; hlt` too.
- **Memory-map boundaries must agree across files** — change one boundary, change
  the others (see Memory model).
- **Syscall numbers must stay in sync** between `syscall.c` and `user/lib/lib.c`.
- **Never auto-format AHCI/NVMe** — that is the user's real OS drive (see Storage).
- **Syscalls stay general primitives**, never per-feature shortcuts; pure
  computation (codecs/decompression) goes in `user/lib`, not a syscall.

## Code conventions

- **Build general components, not single-use ones (unless you think it could be re-used, example: UUIDs).**
  A file written while working toward feature X is built *because of* X, not *for* X: keep its API
  consumer-agnostic and put feature-specific logic in the caller (e.g. the GC
  takes a generic tracer/finalizer; JS-specific marking lives in the engine).
- **Name for what a thing is, not its first consumer** — no `js_`-style prefixes
  on shared code; prefer the standard libc name when one exists (`dtoa`, `strtod`).
- **Comments are short and load-bearing.** Comment only the non-obvious (a subtle
  invariant, a footgun, why-not-the-obvious-way), one line where possible. No
  banners, no narration of what the code plainly does; default to none.

## Boot & subsystem bring-up (kernel.c)

`boot/boot.S` (Multiboot header, masks IRQs, sets stack, passes magic+info) →
`kernel_main`, which brings subsystems up in dependency order:

serial → vga → gdt → idt → irq (PIC remap) → pmm → **paging** → heap → smbios →
pci → **smp_init (MADT CPU discovery)** → **fb** → net (NIC dispatch + IPv4) →
ahci/nvme probes → **blk (pick owofs disk)** → fs → syscalls (+install user
programs) → timer → keyboard → sb16/hda → scheduler + workers → services →
**wm** → screenshot worker → **smp_start_aps** → `sti` → **shell**.

- **IDT before any IRQ.**
- **SMP (smp.c + smp_ap.S):** `smp_init` discovers cores via the ACPI MADT early;
  `smp_start_aps` wakes the application processors just before `sti` — they
  currently **park** (boot stays effectively uniprocessor; per-core scheduling is
  the remaining work). `make run SMP=N` sets the core count QEMU exposes.
- **PIC remapped (master→32, slave→40) before `sti`**, or the timer IRQ lands on
  a CPU exception vector.
- **`blk_init` after ahci/nvme** (needs their `_present()`) **and before fs_init**.
- **`xhci_boot` after `sti`** (uses `timer_sleep`, needs interrupts on).
- Slow post-`sti` work runs off the main thread (`xhci_boot` in the detached
  `boot_bringup` thread; DHCP as the autostart `dhcp` service) so boot keystrokes
  don't pile up in the keyboard ring; both still run IF=1.
- Device probes are no-ops when the hardware is absent. **Nothing auto-mounts.**

## Boot loaders, Multiboot, firmware, graphics (boot.S + kernel.c + fb.c)

- `boot.S` carries **two** headers. **Multiboot1** (`0x1BADB002`) for QEMU
  `-kernel` (hands us `multiboot_info` directly). **Multiboot2** (`0xE85250D6`)
  for GRUB (BIOS+UEFI) — GRUB's mb1 handoff page-faults during the long→protected
  transition under UEFI; mb2 is the handoff GRUB does correctly.
- Both request a **32-bpp** linear framebuffer at `0×0×32` (no fixed resolution):
  a hardcoded mode makes GRUB fail `no suitable video mode found`; depth is pinned
  to 32 because `fb_adopt` only handles 32-bpp XRGB.
- On mb2, `multiboot2_to_info()` walks the tag chain and synthesizes an mb1
  `multiboot_info` so pmm/fb need no per-format code. Keep `synth_mmap` large —
  real UEFI hands out hundreds of descriptors; too small drops the bulk RAM
  region and pmm sees ~0 MiB.
- While walking it **detects firmware** (`firmware.c`) and **captures the RSDP**
  (`acpi_set_rsdp`) — under UEFI the RSDP isn't in the legacy BIOS area, so this
  is the only route the ACPI code has to the tables.
- Graphics: if the loader gave a 32-bpp RGB framebuffer, **adopt it** (`fb_adopt`);
  else program QEMU's DISPI adapter (`fb_init`); else stay in text. Test adopt
  with `-vga virtio`.
- `grub.cfg` **must `insmod all_video`** or the framebuffer request can't be
  honored. **Known limitation:** GRUB 2.14's relocator page-faults under
  strict-W^X OVMF (inside GRUB) for both mb1 and mb2; a native EFI stub is the
  only fully robust UEFI path — not yet done.

## Memory model (paging.c + loader.c + heap.c)

Low 16 MiB is identity-mapped, partitioned by hardcoded addresses (**must agree
across files**):

```
0–~2 MiB    kernel image + BSS (PMM bitmap, page tables, stacks)  [supervisor]
2–4 MiB     spare identity-mapped low memory (was the old user window)
4–12 MiB    kernel heap (kmalloc/kfree)
12–16 MiB   page-frame pool (palloc/pfree: page directories + page tables)
```

- The per-process **user window is a private 64 MiB region at 1 GiB**
  (`USER_BASE 0x40000000 .. USER_REGION_END`, loader.h), backed on demand by pmm
  high memory (>16 MiB) through the process's own page tables. So `palloc`/`pfree`
  hand out only page dirs/tables; user *frames* come from the PMM.
- User programs link at `0x40000000` (`user/user.ld`); the stack grows down from
  `USER_STACK_TOP`. 1 GiB is clear of the identity map, of `SCRATCH_VADDR`
  (256 MiB — a per-CR3 slot to transiently map+write any pmm frame), and of
  `WIN_FB_VADDR` (3 GiB — a window's client pixel buffer in its owning process).
- **Above 16 MiB is on-demand:** `pmm_alloc_contig` hands out contiguous frames
  (and identity-maps them) for things too big for the heap (fb back buffer, tmpfs).
- Two helpers map outside the identity map, both into the shared kernel directory:
  `paging_identity_map(phys,len)` for high firmware tables (ACPI),
  `paging_map_uncached(phys,len)` (UC-) for device MMIO (e1000 BAR0).
- Each process has its **own page directory** (shared kernel/heap/pool + private
  user window); the scheduler switches CR3 and `tss.esp0` per task. **Every
  address space maps the kernel identically**, so kernel threads and interrupt
  handlers run under any CR3 — why the scheduler needn't special-case address
  spaces.

## Graphics & window manager (fb.c → wm.c, with mouse.c + console.c)

- fb gets the LFB two ways: program QEMU's VBE DISPI registers (ports
  0x1CE/0x1CF, LFB via VGA PCI BAR0), or **adopt** the bootloader's framebuffer.
  `kernel_main` prefers adopt, falls back to DISPI; with no framebuffer the kernel
  stays in 80×25 text mode (the **recovery console**, shell.c, over COM1).
- **`con_putc` (console.c) is the one funnel** every console write passes through;
  it fans out to serial, the journal (log.c), the fb boot console, an optional
  capture buffer, and the 80×25 VGA text shadow grid.
  `con_use_framebuffer(1)` stops the text backend touching 0xB8000.
- The text grid is the recovery console; the **desktop terminal is the separate
  `/bin/term`**, which renders its own grid.
- **wm.c is one kernel thread** (draggable title bars, raise-on-click z-order,
  focus, close boxes, cursor, clock/taskbar); paces itself with `timer_sleep`.
- **Mouse shares the i8042 with the keyboard (mouse.c, IRQ12).** `mouse_init`
  force-sets both IRQ + clock bits and **must preserve bit 6 (translation)** or
  keyboard.c stops seeing set-1 codes. IRQ12 is on the slave PIC → `irq_unmask(2)`
  (cascade) too.

**Performance is load-bearing on real hardware** (QEMU hides it). Three fb.c rules
took a laptop from 2 fps to smooth:

1. **Back buffer in cached system RAM**, from `pmm_alloc_contig` above 16 MiB —
   never VRAM, never single-buffered.
2. **`fb_present` copies only the damaged rectangle** (clip rect + dirty box; the
   compositor diffs each frame). A full-screen present every frame is 2 fps.
3. **Front buffer mapped write-combining** (`enable_wc`: PAT entry 1 = WC,
   `wbinvd`, `paging_mark_wc` tags LFB PTEs) — ~10× over default UC. The back
   buffer stays write-back cached.

## Desktop apps & windowing (apps.c + uwin.c + crash.c + screenshot.c)

- **`apps_start()` (apps.c) launches three programs at boot:** `clipd` (clipboard
  server, windowless), `notifyd` (notification server, resident; draws the
  Notification Center on demand), then `/bin/term`. Everything else launches from
  the taskbar Start menu (`app_menu[]`: Terminal, Files, Editor, Browser, Media
  Player, Clock, Calculator, Notifications, Settings, About). Apps are `/bin`
  programs spawned detached and reaped by the WM; the only kernel-side window is
  the About box. (imgview is built and installed but reached via Files / browser,
  not the Start menu.)
- **The terminal *is* the shell (`user/apps/gui/term.c`)** — own line editing,
  history, cwd, dispatch; **no kernel-shell bridge.** Commands run three ways:
  builtins over syscalls; CLI tools (neofetch/curl/lspci/ping/dig/play/torrent)
  spawned with stdout captured via `spawn_cap` and rendered inline like a pty; any
  other `/bin` program detached.
- The kernel `shell.c` survives only as the COM1/text **recovery console** (its
  builtin table still has kernel-only ops like `format`, `mtp`).
- **GUI apps own a window via uwin.c** (the user-window server): create a window,
  get a shared pixel buffer mapped at `WIN_FB_VADDR` (3 GiB), draw, present, poll
  input events — all over `SYS_WIN_*`.
- **Ctrl+C is a `KEY_INTR` key event** delivered to the focused app (kernel.c
  routes the latch via `wm_feed_key`): cancels the line, or kills a captured child.
- **Keyboard routing:** `wm_feed_key` offers a key to the focused GUI app first,
  falling back to the recovery console's `shell_feed` only when no window has focus.
- **crash.c contains a user-process fault, never panics:** the IDT handler calls
  `crash_record()` in the faulting task's context, captures registers, kills the
  process; the WM's `crash_pump()` turns it into an "<app> has crashed" window.
- **screenshot.c:** the keyboard IRQ only *requests* a Print Screen shot; a worker
  thread does the capture (`/home/root/screenshotN.png`).

## Process model & scheduling (sched.c + switch.S)

Tasks are nodes in a circular run queue; `schedule()` is the timer-tick callback
and the only normal entry to a context switch.

- **switch.S stack layouts are exact.** New kernel threads, new user tasks
  (`enter_user_task`), forked children (`fork_child_entry`) each have a
  hand-crafted initial kernel stack so the first `context_switch` returns into the
  right trampoline. Off-by-one = triple fault.
- **fork** copies the address space + syscall trap frame, forcing the child's
  saved `eax` to 0.
- **The image-replace syscall** rewrites the trap frame (eip/esp/segments) and
  swaps CR3 *before* freeing the old address space.
- **exit** makes a zombie; the parent (`waitpid` / `sched_try_reap`) frees it.
  Blocking syscalls yield rather than busy-spin.
- **`timer_sleep` deschedules** (`sched_sleep_until`): a sleeper goes
  `TASK_WAITING` with a `wake_tick`, woken by `sched_wake_due()` each tick — not
  kept `RUNNABLE` and `hlt`-spun (that gave O(N-threads) wakeup latency and lagged
  the UI with many windows). The **main task never sleeps**, so it's the
  always-runnable fallback. (Network waits use their own `sti; hlt` loops.)
- **`MAX_TASKS` is 64** — the steady set plus an app's burst (the browser is
  process + fetch worker + one thread per inline image); at 16 a fresh browser
  starved other spawns. Only `tasks[]` scales; each kstack is `kmalloc`'d on demand.
- **EOI is sent at the start of `irq_handler`** — a timer handler that
  context-switches away never reaches the end.
- **Kernel-thread stacks are 16 KiB** (`KSTACK_SIZE`), bumped from 4 KiB for the
  SMB server (deep handlers + on-stack segment buffer + nested interrupt frames).

## Syscalls, /proc, /sys, user programs (syscall.c + loader.c + user/)

- ABI: `int 0x80`, number in `eax`, args in `ebx/ecx/edx`, return in `eax`. All
  user pointers are bounds-checked against the user window.
- The net-* calls are a thin connect/send/recv/close handle layer (`net_sock_*`)
  — the only user-facing network API; `user/apps/cli/curl.c` is the client. `net_connect`'s
  `proto` arg selects TCP/TLS/UDP/raw-ICMP, so ping/dig/DNS are user-space code
  over sockets, not bespoke syscalls.
- **Kernel state is exposed through two synthetic filesystems** (the Unix way),
  via the ordinary file API — no bespoke syscalls.
- **`/proc` (procfs.c) is flat, one fact per file.** Reads expose system info
  (`cpuinfo meminfo uptime date storage tasks pci host firmware battery display
  journal services usb net clipboard`); writes are **control nodes** that trigger
  a kernel action (text to `/proc/clipboard`, `start X`/`stop X` to
  `/proc/services`, a DHCP/NTP re-run via `/proc/net`).
- **`/sys` (sysfs.c) is a real nested hierarchy** of kernel objects + attributes,
  walkable at every level — where *device tunables* live:

```
/sys/class/input/kbd0/layout     keyboard layout      (rw, loadkeys)
/sys/class/input/mouse0/speed    pointer speed 1..5   (rw, Settings)
/sys/class/net/eth0/{address,carrier,ipaddr}          (ro)
/sys/class/block/<dev>/{model,size}                   (ro)
/sys/bus/pci/devices/<bdf>/{vendor,device,class}      (ro, one dir per device)
/sys/power/battery/{capacity,status}                  (ro, real HW)
/sys/firmware/type               BIOS / UEFI          (ro)
```

- Choice follows Linux: info → `/proc`, device attribute → `/sys`. fs.c routes
  `/sys` for ls/stat/read; syscall.c routes writes. Dynamic parts (which
  NIC/block devices, battery presence) are an explicit per-level path walk.
- A genuine *action* that returns nothing stays a syscall (e.g. `SYS_POWER`). The
  terminal captures a child's stdout via `SYS_SPAWN_CAP` (+ `read_nb`/`kill`).
- Each task carries a **cwd** (`task_t.cwd`); a parent sets a child's via
  `sched_set_cwd`, fork/`spawn_cap` inherit it, `SYS_CHDIR` sets the caller's, and
  path syscalls `join_cwd` relative names before hitting owofs.
- User programs are freestanding ELF32 (`user/apps/{gui,cli}/NAME.c`) linked at
  `0x40000000` (`user/user.ld`). Build: compile → `user/apps/.../NAME.elf` →
  objcopy renames the blob symbols to `_binary_app_NAME_{start,end}` →
  `kernel/NAME_blob.o` → linked in. **Adding a program = two edits** (`NAME_blob.o`
  in `kernel/build.mk` OBJS + an `INSTALL_APP(NAME)` line in `kernel_main`); a
  generic `%.elf` rule covers plain apps, so a *third* edit (a link rule in
  `user/apps/build.mk`) is needed only if the program pulls in extra libs. No
  shell wiring — `run()`'s final `else` spawns any non-builtin as a `/bin` program.
- **Install/refresh:** owofs (`disk.img`) persists installed `/bin` programs.
  `install_blob()` reinstalls when absent **or its on-disk size differs from the
  built-in blob**, so a rebuild replaces the persisted binary next boot — no
  `make distclean` needed (except the rare same-byte-size behavior change).
  `/etc/passwd`+`/etc/group` stay install-if-absent so accounts persist.

## Storage: block dispatch & filesystem (blk.c + fs.c + drivers)

- owofs (fs.c) is a flat 512-byte-sector filesystem with a directory tree.
  `blk.c` picks **one** backing device at boot — first present of IDE → AHCI →
  NVMe (`blk_backend()` names it) — and routes fs.c's sector I/O to it.
- **Auto-format only a blank IDE disk** (`blk_is_ide`); fs_init **never**
  auto-formats AHCI/NVMe (the user's real OS drive).
- **tmpfs fallback:** with no usable owofs disk, fs_init calls `blk_use_ram()` → a
  volatile RAM disk above the identity map (`blk_backend()` → `"tmpfs"`).
- **Removable volumes mount under `/media/<name>`** via a multi-instance mount
  table (fsmount.h): several at once, including duplicates. Each driver
  (fat/hfs/ext/iso9660/ntfs) is a `struct fs_ops` vtable over an opaque per-mount
  instance; fat.c is read+write, the rest read-only. `mount_for()` routes a path by
  longest-prefix; `/media` and `/dev` are synthetic.
- **Nothing auto-mounts** — `mount <dev> [point]` / `umount <point>` or the Files
  app. Drivers probe by signature and never auto-format.
- `blk_device_list()` reports *present* removable devices (candidate order CD →
  USB → IDE → SATA → NVMe → SD, purely each driver's `_present()`, no sector I/O);
  `blk_mount_device(name)` mounts one.
- **Hotplug is live where the hardware allows:** the xHCI poll thread toggles
  `msc_ready`, `sd_present()` reads the SDHCI card-detect bit each call; IDE/SATA/
  NVMe presence is latched at boot.
- **Real-hardware caveat:** a flaky USB-storage adapter can wedge the xHCI
  controller; an MMIO read to a controller that's fallen off the bus hangs the CPU
  bus cycle, which no software timeout catches — so `mount`/`storage` can freeze
  the machine (a hardware fault). Every *software* wait in the USB path is bounded.

Per-driver pointers (internals live in the source):
- **sd.c** — SD/MMC over an SDHCI host controller (PCI class 0x08/0x05), polled
  PIO one 512-byte block at a time; `sd_init` runs before the timer so it busy-spins.
- **atapi.c** — CD/DVD over IDE by PACKET, 2048-byte sectors; QEMU lands `-cdrom`
  on the secondary IDE master.
- **iso9660.c** — read-only ISO 9660 / Joliet on atapi.c (sector source is the
  2048-byte `atapi_read_sector`), so blk.c mounts the CD directly.

## Network stack (nic.c → net.c → tcp.c → tls.c, with crypto.c)

- Kernel-side stack; `ping`/`dig` are `/bin` tools over `net_sock_*`. The one
  user-space client is **curl** (`user/apps/cli/curl.c`): HTTP/1.1, HTTP/2
  (`--http2`, `user/lib/curl/h2.c`), HTTP/3 (`--http3`, `user/lib/curl/{quic,h3}.c`),
  all in user space (no Berkeley sockets; doesn't advertise gzip).
- **nic.c — link-layer dispatch.** Each driver exports `struct nic_ops`;
  `nic_init` probes `{ igb, e1000, rtl8139 }` and keeps the first present.
  `nic_set()` installs a hot-plugged driver at runtime.
  - rtl8139.c — the QEMU default.
  - e1000.c — Intel **I217–I219** (PCH e1000e/ich8lan), *not* I210/I211: MMIO
    register file (UC-), legacy RX/TX rings, RX IRQ. `make run NIC=e1000`.
  - igb.c — Intel **I210/I211/82576/82580/I350**: a different MAC (per-queue
    descriptor windows, advanced descriptors, per-queue ENABLE handshake), RX
    drained off the timer tick (no RX IRQ). I211 (0x1539) is the real-HW target;
    QEMU's 82576 is the in-emulation proxy for the same code path. `make run NIC=igb`.
- **net.c — IPv4:** ARP (cached), ICMP echo, UDP (DNS); DHCP and NTP are now
  services (`dhcp` autostarts the boot lease, `ntp` on demand).
- **tcp.c** — active + passive open, several concurrent TCBs demuxed by 4-tuple; a
  SYN is accepted in the IRQ and handed to a daemon thread. In-order only, retx
  only SYN/SYN-ACK, no congestion control, no TIME_WAIT — fine over SLIRP.
- **tls.c + crypto.c** — TLS 1.3 (1.2 fallback), X25519 + AES-128-GCM +
  SHA-256/HMAC/HKDF, freestanding. **The server cert is not verified** —
  deliberate: resists passive eavesdropping, not active MITM.
- **RX runs in interrupt context**, so blocking client APIs (`tcp_read` …)
  `hlt`-spin while the NIC IRQ fills buffers, rather than touching the wire.
- **QUIC + HTTP/3 are entirely user space** (QUIC v1 over `SK_UDP`, own TLS 1.3 in
  CRYPTO frames + QPACK static-table). One connection at a time, cert not verified,
  loss-free assumed. Two **general** kernel enablers in net.c: the UDP socket
  buffers a *ring* of datagrams (`SOCK_DGRING`), and `net_udp_send`'s cap rose from
  512 to a full MTU (QUIC's Initial must be ≥1200 bytes).

## Services (services.c + UDP hooks in net.c)

Headless daemons in a fixed enum-indexed registry (`on_start`/`on_stop`/`tick`).

- **One supervisor thread for cheap services, a dedicated thread per stream one.**
  "servicesd" round-robins `tick` over RUNNING services; start/stop just flips a
  flag. UDP daemons (`echod`, `daytimed`) have no tick — they serve from the RX
  IRQ. TCP daemons (`echotcp`, `smbd`) own a thread looping `tcp_accept → serve →
  close`; `on_stop` trips a flag to wake a blocked `tcp_accept`.
- **UDP daemons serve from interrupt context:** `net_udp_listen(port, fn)`
  registers a handler; `udp_recv` demuxes by dest port and replies with
  `net_udp_send` (safe from the IRQ). Keep `udp_recv`'s call site in `ip_recv` in
  sync (it takes the source IP).
- **One-shot services** (`dhcp`, `ntp`) have no tick/port — their `on_start` *is*
  the action. `dhcp` autostarts and spawns an async client thread; `ntp` is
  on-demand. Re-trigger with `services restart <name>`.
- Adding a service = one registry edit (+ an accept-loop thread for a TCP one).
- Testing needs a `hostfwd` (SLIRP blocks inbound; `make run`/`debug` bake in
  `hostfwd=tcp::1445-:445` for smbd); guest IP is 10.0.2.15.

## Subsystem pointers (internals live in the source)

- **SMB server (smb.c, the `smbd` service)** — read-only SMB2 (dialect 2.0.2)
  exposing owofs as share `owofs`, one client at a time. **Auth is cosmetic**
  (accepts any credentials as guest). `smbclient -L` won't work (no IPC$ srvsvc
  pipe) — connect to the share directly.
- **BitTorrent (user/lib/torrent)** — from-scratch client entirely in user space
  (`/bin/torrent info|peers|peer|get`), TCP-only over `tcp_connect`/`tcp_read`.
- **SQLite (user/lib/sqlite)** — from-scratch reader/writer for the real SQLite3
  file format; in-memory model (whole-file `sql_open`/`sql_save`). Scope: table
  b-trees + NULL/int/float/TEXT/BLOB; **no index b-trees, overflow pages, or WAL**
  (one record must fit a 4 KiB page). First user is the browser cookie+HSTS jar
  (`user/apps/gui/browser/jar.c`, `/home/root/cookies.db`); SQL stays out of fetch.c.
- **HTTP headers (user/lib/curl/headers.c)** — response headers parsed once into a
  k/v object (case-insensitive, repeats kept in order), shared by curl + browser.
  The browser also honors `X-Content-Type-Options: nosniff`.
- **HTTP cache (user/lib/curl/httpcache.{c,h} + browser cache.{c,h})** — the RFC 7234
  caching *policy* (HTTP-date parse, Cache-Control/Expires/Age/Date/ETag/Last-Modified
  → storable/freshness/current-age) is a pure, host-testable lib; the browser's
  `cache.c` is the store built on it (LRU, size-capped, spinlocked for the parallel
  sub-resource pool). Wired into `fetch.c` (document, all transports, incl. 304
  conditional revalidation) and `resource.c` (`browser_fetch` + the h2-multiplex
  `subres_job`/`h2_prefetch` — `h2_fetch_many` gained optional per-stream header
  capture so multiplexed sub-resources are cacheable). **Persistent across sessions:**
  each entry's body is **one file per entry** under `/home/root/.cache/browser` (NOT
  SQLite — its jar records cap at a 4 KiB page, too small for bodies), with an
  in-memory metadata index. owofs allows one open write stream at a time, so disk
  writes/deletes are **deferred to `cache_flush()`** on the single worker thread after
  a navigation (jar.c's flush pattern); `cache_load()` restores at browser startup.
  Verify the policy with `scripts/httpcache_test.c`.
- **Notifications (user/lib/notify + notifyd + notify-send)** — `user/lib/notify`
  is the client lib; `notifyd` (`user/apps/gui/notifyd.c`) is a resident daemon
  listening on the `notify` IPC port, keeping history and drawing the Notification
  Center; `notify-send` (`user/apps/cli/notifysend.c`) is the shell client. Wire
  protocol is length-prefixed strings; a button can run a shell command on click.
  A second `notifyd` launch just asks the resident one to open its window.
- **Media (user/lib/media + player + browser `<audio>`/`<video>`)** — two layers:
  a decoder (WAV/MPEG → 48 kHz/S16/stereo) + a **user-space software mixer** that
  merges concurrent sources on a background thread, because the kernel exposes a
  single exclusive PCM stream. `user/apps/gui/player.c` is the standalone player;
  the browser wires it through `bmedia.h`. Files decode incrementally; video
  elements are accepted but not decoded (black frame). Decode faults are
  user-space crashes only.
- **WebP (user/lib/webp + webp CLI)** — from-scratch VP8L **lossless** codec
  (encode + decode), in-memory; wired into imgview, the browser, and the `webp`
  CLI (info/encode-from-PNG/verify). Lossy VP8 and animation are rejected
  (`WEBP_EUNSUP`).
- **JSON (user/lib/json + json CLI)** — RFC 8259 parser/encoder building a
  malloc'd value tree (numbers are IEEE-754 doubles); `json` CLI
  validates/pretty-prints/minifies. Pure libc + libmath, no engine coupling.
- **CORS (user/apps/gui/browser/cors.{c,h})** — pure stateless policy logic (origin
  extraction, same-origin/same-site, preflight validation) for the browser's two
  guards: CORS (cross-origin read) + CORP (cross-origin sub-resource embedding);
  `resource.c` orchestrates fetch/XHR around it. Registrable-domain check
  approximates the public suffix list with the last two dotted labels.
- **Brotli (user/lib/brotli)** — from-scratch RFC 7932 decoder + encoder, user
  space, whole-buffer, for the HTTP `br` content-coding; verified vs the system
  `brotli` CLI. The browser's HTTP/1.1 path advertises and decodes `br`.
- **ACPI/power/battery (power.c + aml.c + battery.c)** — mostly real-HW.
  `power_off()` does ACPI S5; it finds the RSDP (legacy scan, or the mb2 copy from
  `acpi_set_rsdp` — the only route under UEFI), walks XSDT/RSDT → FADT → DSDT.
  aml.c is a tiny best-effort interpreter scoped to the battery; **fails gracefully**
  (returns 0, never crashes). Battery is real-HW-only — keep its diagnostics.
- **USB host & tethering (xhci.c + usb.c)** — `lsusb` over UHCI + xHCI; `xhci_boot`
  keeps the host persistent. Goal: phone tethering via an RNDIS driver registering
  as a `nic_ops`. **Tethering does not work on real hardware** — not a usable
  fallback; the wired NIC is the real path to laptop internet.
- **Audio (drivers/audio: speaker.c + sb16.c + hda.c)** — `play <file.wav>` prefers
  HDA (`hda_present()`) else SB16; both share the RIFF/WAVE parse + Ctrl+C stop.
  `hda_init` runs before `sti` so it busy-waits; DMA buffers from `pmm_alloc_contig`
  must be `paging_identity_map`'d before first touch; volume/Fn keys run from the
  keyboard IRQ (drive HDA when present, else the SB16 mixer). Real Realtek codec
  quirks are real-HW-only.
