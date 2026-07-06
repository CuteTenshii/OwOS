# CLAUDE.md

OwOS is an x86 Unix-like hobby kernel written from scratch in C + GAS assembly,
booted via QEMU Multiboot. One source tree builds 32-bit (`i386`, the default and
primary target) and 64-bit (`make ARCH=x86_64`); notes assume 32-bit unless
stated. README.md has the feature list and per-file layout — this file covers
only what spans files or isn't obvious from one read.

**Source layout:** drivers live under `drivers/<type>/`
(`storage net audio usb input video acpi bus`); the core kernel, filesystems
(`fat/hfs/ext/iso9660/ntfs`), block dispatch (`blk`) and the network stack
(`net/tcp/crypto`) stay in `kernel/`. User space splits into `user/lib/<name>/`
(reusable libs) and programs in `user/apps/gui/` (windowed) + `user/apps/cli/`
(terminal), each embedded in the kernel as a blob.

- Includes are **flat** (`#include "fb.h"`, never a path): the build puts
  `kernel/` and every `drivers/` subdir on `-I` (`DRVDIRS`, in toolchain.mk), so
  moving a driver between folders is an OBJS path change only. **Header basenames
  must stay unique.**
- **The build is one non-recursive graph**, root-relative: the `Makefile`
  `include`s `toolchain.mk` (flags + arch select), `user/lib/build.mk`,
  `user/apps/build.mk`, `kernel/build.mk` (OBJS + link) and `run.mk` (QEMU).

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
make run NIC=e1000  # attach QEMU's e1000 instead of the default RTL8139
make run SMP=8      # expose 8 cores to the guest (default 4; SMP=1 = uniprocessor)
make run ACCEL=tcg  # force pure emulation (default uses KVM/HVF, ~10-50x faster)
```

- No cross-compiler on Linux: freestanding ELF (`gcc -m32`/`-m64 -ffreestanding
  -nostdlib`) linked at 1 MiB by `linker.ld`/`linker64.ld`, loaded via QEMU
  `-kernel`. **User programs are always 32-bit** even under an x64 kernel (each is
  an opaque blob), so user code uses the i686 toolchain.
- `make run`/`debug` auto-create every disk a storage driver exercises (IDE
  `disk.img`, AHCI, NVMe, USB stick, SD card, ISO 9660 `cd.iso` on ATAPI CD), a
  SLIRP NIC and a USB tablet; `make run` also adds SB16 + Intel HDA.
- rtl8139/e1000/igb are all built in; the kernel keeps whichever QEMU attached.
- `make iso` is the only target needing a host bootloader (`grub-mkrescue`).
- clang editor diagnostics analyze in 64-bit mode and flag every pointer/int
  cast: **false positives** under the real `-m32` build; ignore them.

## Verifying changes

No test framework — the kernel is verified by **booting in QEMU and watching real
behavior**. A bad change usually triple-faults (silent reboot loop) rather than
erroring, so a clean compile proves nothing.

**Scripted runs: `scripts/qemu-test.sh`.** Boots `owos.bin` headless, types
command lines into the COM1 recovery console over serial (plain text, *not*
`sendkey`), and prints the serial log. Runs against a throwaway copy of
`disk.img` (no write-lock — safe alongside a live `make run`); a hard timeout
caps hangs. `--` separates flags from the command list:

```sh
scripts/qemu-test.sh -- "run hello" "ls -l /home/root"
scripts/qemu-test.sh --nic e1000 --boot-wait 20 -- "ping 10.0.2.2"
```

Flags: `--boot-wait --gap --drain --nic --no-net --smp --accel --kernel --disk
--log --timeout`. Exit 124 is normal (the fixed-length session drains to the end).

**Fault triage:** drop to raw `qemu-system-i386 -kernel owos.bin -hda disk.img
-monitor stdio` and drive it by hand when you need monitor commands the harness
can't send — `info registers`, `xp` (physical mem), or framebuffer `sendkey`.
A reboot loop is a triple fault: rerun with `-no-reboot -d int,cpu_reset`. On a
fault, idt.c panics (vector, error code, EIP, CR2, and the present/write/**user**
bits that say whether it came from ring 3) to COM1 and a heapless fb screen.

- Never pass `-vga std`: it unmaps the 0xB8000 text window and nothing renders.
  The default adapter keeps SeaBIOS's text mode 3.
- **macOS:** `timeout` is GNU-only (use `gtimeout`); the build needs an ELF
  cross-toolchain (`i686-elf-` / `x86_64-elf-`, auto-selected per ARCH). See README.
- The battery and xHCI/tethering paths are **real-hardware-only**; keep their
  diagnostic plumbing (`battery_error`, `aml_last_error`, per-port xHCI lines).
- Remove any deliberate-fault test you add, and re-verify a clean boot.

**Pure-lib logic gets host unit tests in `scripts/`, no QEMU.** When a `user/lib`
file is pure computation (RFC policy, codec math), compile it against host libc
and assert directly — far faster than a boot. `scripts/owos.h` is a host shim
supplying the `user/lib/owos.h` decls the freestanding source pulls
(`strlen`/`malloc`/`lower`/`starts_ci` …), so `-Iscripts` lets the real `-m32`
source compile natively. `httpcache_test.c` is the worked example (its header
comment carries the exact `cc` line):

```sh
cc -std=c11 -Iscripts -Iuser/lib/curl \
   scripts/httpcache_test.c user/lib/curl/httpcache.c user/lib/curl/headers.c \
   -o /tmp/httpcache_test && /tmp/httpcache_test
```

## Hard rules

The rules most likely to be violated:

- **`exec` + `(` is blocked by a repo hook** (misfires as a Node child-process
  call). So the kernel fn is `sched_replace`, the libc wrapper is `execp`, and
  comments say "the run-image syscall" — never spell it with a parenthesis.
- **`sti` before `hlt` in every thread-context wait loop.** `context_switch`
  doesn't save/restore EFLAGS, so a thread resumed by a cooperative `sched_yield`
  from an IF-clear context inherits IF=0; a bare `hlt` then halts the CPU forever.
  The `int 0x80` gate also runs IF=0, so any syscall path that spins on `hlt` must
  `sti; hlt` too.
- **The fixed high virtual-address constants must stay mutually clear** — change
  one, check the others (see Memory model).
- **Syscall numbers must stay in sync** between `syscall.c` and `user/lib/lib.c`.
- **Never auto-format AHCI/NVMe** — that is the user's real OS drive (see Storage).
- **Syscalls stay general primitives**, never per-feature shortcuts; pure
  computation (codecs/decompression) goes in `user/lib`, not a syscall.
- **The JS GC (`user/lib/gc`) is generational: call `gc_write_barrier(container)`
  after storing a GC pointer into an *object/env*.** A minor collection never
  traces the old generation, so an unbarriered old→young pointer is a
  use-after-free. The barriered sites live in `value.c`/`interp.c`/`builtins.c`
  (`js_def`/`js_set`/`js_def_accessor`/`js_env_define`/`assign_ident`, plus
  `proto`/`fn->home`/`fn->data` stores) — any *new* such store needs one too.
  Arrays are exempt (kind `GCK_ARRAY`, re-scanned wholesale each minor). Verify
  with the torture harness: build the host JS test `-DGC_TORTURE -fsanitize=address`
  (collects on every alloc) and run the corpus — a missing barrier trips ASan.

## Code conventions

- **Build general components, not single-use ones** (unless plausibly reusable,
  e.g. UUIDs). A file written *toward* feature X is built because of X, not for X:
  keep its API consumer-agnostic and put feature-specific logic in the caller
  (the GC takes a generic tracer/finalizer; JS-specific marking lives in the engine).
- **Name for what a thing is, not its first consumer** — no `js_`-style prefixes
  on shared code; prefer the standard libc name when one exists (`dtoa`, `strtod`).

## Boot & subsystem bring-up (kernel.c)

`boot/boot.S` (Multiboot header, masks IRQs, sets stack, passes magic+info) →
`kernel_main`, which brings subsystems up in dependency order:

serial → console → gdt → idt → irq (PIC remap) → pmm → **paging** → heap → smbios
→ pci → **smp_init (MADT CPU discovery)** → **fb** → net (NIC dispatch + IPv4) →
ahci/nvme/sd/atapi probes → **blk (pick owofs disk)** → fs → syscalls (+install
user programs, seed `/etc/inittab`) → timer → keyboard → sb16/hda → scheduler +
workers → mouse → `sti` → **smp_start_aps** → `boot_bringup`
thread → **shell** (recovery console).

- **The desktop and daemons come up in user space.** `boot_bringup` spawns
  `/bin/init` (PID 1, uid 0), which reads `/etc/inittab`
  (`compositor\ndhcpcd\nmdnsd\nsshd`) and launches each `/bin` daemon detached —
  the kernel runs no window manager and starts no daemons. The compositor comes
  first so the desktop is up before the network daemons.
- **IDT before any IRQ. PIC remapped (master→32, slave→40) before `sti`**, or the
  timer IRQ lands on a CPU exception vector.
- **SMP (smp.c + smp_ap.S):** `smp_init` discovers cores via the ACPI MADT early;
  `smp_start_aps` wakes the APs just after `sti` — they currently **park** (boot
  is effectively uniprocessor; per-core scheduling is the remaining work).
  `make run SMP=N` sets the core count QEMU exposes.
- **`blk_init` after ahci/nvme** (needs their `_present()`) **and before fs_init**.
- **`xhci_boot` after `sti`** (uses `timer_sleep`, needs interrupts on); it runs
  in the detached `boot_bringup` thread (after spawning init), alongside
  `vmtools_init` (VMware guest agent: clock/clipboard sync over the backdoor).
- Slow post-`sti` work runs off the main thread so boot keystrokes don't pile up
  in the keyboard ring; DHCP is the user-space `/bin/dhcpcd` launched by init.
  Everything here still runs IF=1.
- Device probes are no-ops when the hardware is absent.

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
  region and pmm sees ~0 MiB. While walking it **detects firmware**
  (`firmware.c`) and **captures the RSDP** (`acpi_set_rsdp`) — under UEFI the RSDP
  isn't in the legacy BIOS area, so this is the ACPI code's only route to it.
- Graphics preference order in `kernel_main`: **`vmsvga_init` (VMware SVGA II)** →
  **adopt the loader's 32-bpp framebuffer** (`fb_adopt`) → **program QEMU's DISPI
  adapter** (`fb_init`) → stay in text. Test adopt with `-vga virtio`.
- `grub.cfg` **must `insmod all_video`** or the framebuffer request can't be
  honored. **Known limitation:** GRUB 2.14's relocator page-faults under
  strict-W^X OVMF for both mb1 and mb2; a native EFI stub is the only fully robust
  UEFI path — not yet done.

## Memory model (pmm.c + paging.c + loader.c + heap.c)

The low identity-mapped region splits into kernel image / heap / page-frame pool,
but **the boundaries are computed at boot by pmm.c, not hardcoded** — they
RAM-scale (a ~16 MiB layout on a small box, up to a 192 MiB low region on large
RAM), and the heap starts at the linker's `kernel_end`, *not* a fixed 4 MiB.
The small-box default, for orientation:

```
0–~2 MiB    kernel image + BSS (PMM bitmap, page tables, stacks)  [supervisor]
2–4 MiB     spare identity-mapped low memory
4–12 MiB    kernel heap (kmalloc/kfree)
12–16 MiB   page-frame pool (palloc/pfree: page directories + page tables)
```

The **fixed high virtual-address constants** are what must stay mutually clear:

- The per-process **user window** is a private ~1 GiB region
  (`USER_BASE 0x40000000 .. USER_REGION_END 0x80000000`, loader.h) — code/stack
  low, a large on-demand heap window above — backed by pmm high memory (>16 MiB)
  through the process's own page tables. User programs link at `0x40000000`
  (`user/user.ld`); the stack grows down from `USER_STACK_TOP`.
- `SCRATCH_VADDR` `0x10000000` (256 MiB) — a per-CR3 slot to transiently map+write
  any pmm frame. `WIN_FB_VADDR` `0xC0000000` (3 GiB) — a window's own client pixel
  buffer. `COMPOSITOR_FB_VADDR` `0xD0000000` — the shared back buffer.
  `WIN_MAP_BASE` `0xE0000000` — where `/bin/compositor` maps every window's client
  buffer for compositing.
- **Above 16 MiB is on-demand:** `pmm_alloc_contig` hands out contiguous frames
  (and identity-maps them) for things too big for the heap (fb back buffer, tmpfs).
  So `palloc`/`pfree` hand out only page dirs/tables; user *frames* come from PMM.
- Two mapping helpers, both into the shared kernel directory:
  `paging_identity_map(phys,len)` maps high firmware tables (ACPI);
  `paging_mark_uc(virt,len)` tags already-mapped pages UC- for device MMIO
  (e1000 BAR0) — it takes a vaddr and sets PCD, it does not map.
- Each process has its **own page directory** (shared kernel/heap/pool + private
  user window); the scheduler switches CR3 and `tss.esp0` per task. **Every
  address space maps the kernel identically**, so kernel threads and interrupt
  handlers run under any CR3 — why the scheduler needn't special-case address
  spaces.

## Graphics & framebuffer (fb.c, with mouse.c + console.c)

- fb gets the LFB three ways: VMware SVGA II, program QEMU's VBE DISPI registers
  (ports 0x1CE/0x1CF, LFB via VGA PCI BAR0), or **adopt** the bootloader's
  framebuffer. With no framebuffer the kernel stays in 80×25 text mode (the
  **recovery console**, shell.c, over COM1).
- **`con_putc` (console.c) is the one funnel** every console write passes through;
  it fans out to serial, the journal (log.c), the fb boot console, an optional
  capture buffer, and the 80×25 VGA text shadow grid.
  `con_use_framebuffer(1)` stops the text backend touching 0xB8000.
- The text grid is the recovery console; the **desktop terminal is the separate
  `/bin/term`**, which renders its own grid.
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

## Desktop apps & windowing (compositor.c + uwin.c + crash.c)

- **The user-space `/bin/compositor` is the desktop** — it draws with `gfx_*` into
  the shared back buffer mapped via `SYS_FB_*`. `/bin/init` launches it first from
  `/etc/inittab`; on its first `SYS_FB_MAP` the kernel marks it the compositor
  (`uwin_compositor_active()`) and stops draining the physical keyboard into the
  recovery console. At startup it spawns `clipd` (clipboard server), `vmclipd`
  (VMware clipboard bridge, self-exits off VMware), `notifyd` (notifications) and
  `/bin/term`. Everything else launches from the Start menu (`app_menu[]`:
  Terminal, Files, Editor, Browser, Media Player, Clock, Calculator,
  Notifications, Settings, About) as detached `/bin` programs reaped by the
  compositor. There are **no kernel-side windows**. (imgview is built and
  installed but reached via Files / browser, not the Start menu.)
- **The terminal *is* the shell (`user/apps/gui/term.c`)** — own line editing,
  history, cwd, dispatch; **no kernel-shell bridge.** Commands run three ways:
  builtins over syscalls; CLI tools (neofetch/curl/lspci/ping/dig/play/torrent)
  spawned with stdout captured via `spawn_cap` and rendered inline like a pty; any
  other `/bin` program detached. The kernel `shell.c` survives only as the
  COM1/text **recovery console** (its builtin table still has kernel-only ops like
  `format`, `mtp`).
- **GUI apps own a window via uwin.c** (the user-window server): create a window,
  get a shared pixel buffer mapped at `WIN_FB_VADDR` (3 GiB), draw, present, poll
  input — over the app-side `SYS_WIN_CREATE/PRESENT/POLL/CLOSE`. The compositor
  drives the privileged, root-only side: `SYS_FB_*` (shared back buffer),
  `SYS_INPUT_POLL`/`SYS_INPUT_SET_BOUNDS` (pointer + keyboard), and
  `SYS_WIN_ENUM/MAP/UNMAP/SET_GEOM/DELIVER` (+ `SYS_CRASH_POLL`) to composite and
  route input. **The kernel tracks no focus** — the compositor owns z-order and
  focus, so there is no `SYS_WIN_SET_FOCUS`.
- **Ctrl+C** is a `KEY_INTR` key event the kernel latches and the compositor pulls
  via `SYS_INPUT_POLL` + `DELIVER`s to the focused window. `/bin/term` then
  converts it to `kill(pid, SIGINT)`, so foreground CLI programs receive a real
  signal (ping catches it, ssh forwards `^C`); the recovery console just cancels
  the line.
- **crash.c contains a user-process fault, never panics:** the IDT handler calls
  `crash_record()` in the faulting task's context, captures registers, kills the
  process; the compositor drains records via `crash_poll()` (`SYS_CRASH_POLL`) and
  renders each as an "<app> has crashed" window.
- **Screenshots live in the compositor** (it owns the framebuffer). Print Screen
  is a kernel latch surfaced as a `KEY_PRTSC` input event; the compositor fast-
  captures its back buffer and PNG-encodes + writes `/home/root/screenshotN.png`
  on a worker thread (`user/lib/png/pngenc.c`, uncompressed, shares `$(CRC32)`).
  The old kernel `screenshot.c`/`png.c` are gone: screenshotting is a compositor
  capability, since only the compositor has a composited desktop to capture.

## Process model & scheduling (sched.c + switch.S + signal.h)

Tasks are nodes in a circular run queue; `schedule()` is the timer-tick callback
and the only normal entry to a context switch.

- **switch.S stack layouts are exact.** New kernel threads, new user tasks
  (`enter_user_task`), forked children (`fork_child_entry`) each have a
  hand-crafted initial kernel stack so the first `context_switch` returns into the
  right trampoline. Off-by-one = triple fault.
- **fork** copies the address space + syscall trap frame, forcing the child's
  saved `eax` to 0. **The image-replace syscall** rewrites the trap frame
  (eip/esp/segments) and swaps CR3 *before* freeing the old address space.
- **exit** makes a zombie; the parent (`waitpid` / `sched_try_reap`) frees it.
  Blocking syscalls yield rather than busy-spin.
- **POSIX-style signals (signal.h + sched.c), Phase A and production-grade.**
  Per-task pending/blocked masks + a disposition per signal; `SYS_KILL`,
  `SYS_SIGACTION`, `SYS_SIGPROCMASK`, `SYS_SIGRETURN` with a user mirror
  (`kill`/`sigaction`/`signal`/`sigprocmask`/`raise`, `SIG_*`) in lib.c/owos.h.
  `sched_signal_deliver` builds a user sigframe + trampoline and runs on the
  return to ring 3 (called from idt.c and irq.c); `sched_intr_pending` drives
  EINTR on blocking waits. fork inherits dispositions+mask, exec resets them.
  Job-control signals (STOP/CONT/TSTP/TTIN/TTOU) have reserved numbers but default
  to ignore — Phase B (process groups + controlling terminal) is not wired.
- **`timer_sleep` deschedules** (`sched_sleep_until`): a sleeper goes
  `TASK_WAITING` with a `wake_tick`, woken by `sched_wake_due()` each tick — not
  kept `RUNNABLE` and `hlt`-spun (that gave O(N-threads) wakeup latency and lagged
  the UI). The **main task never sleeps**, so it's the always-runnable fallback.
  (Network waits use their own `sti; hlt` loops.)
- **No fixed task-table cap** — the only ceiling is the kernel heap; `alloc_task`
  fails gracefully and each kstack is `kmalloc`'d on demand. (Bursty spawns need
  this: the browser is process + fetch worker + one thread per inline image.)
- **EOI is sent at the start of `irq_handler`** — a timer handler that
  context-switches away never reaches the end.
- **Kernel-thread stacks are 64 KiB** (`KSTACK_SIZE`): `sshd` runs SSH KEX crypto
  (x25519/ed25519) deep on-stack, on top of nested NIC-RX interrupt frames.
  User-task kernel stacks are 32 KiB (`UKSTACK_SIZE`).

## Syscalls, /proc, /sys, user programs (syscall.c + loader.c + user/)

- ABI: `int 0x80`, number in `eax`, args in `ebx/ecx/edx`, return in `eax`. All
  user pointers are bounds-checked against the user window.
- **Sockets (`net_sock_*`) are the only user-facing network API** — a thin
  connect/listen/accept/send/recv/close handle layer that gives user-space servers
  (`sshd`/`smbd`) passive open. `net_sock_connect(host, port, proto)`'s proto
  selects TCP/UDP/raw-ICMP; raw-Ethernet is a separate `net_raw_open()`. TLS is
  user-space (`user/lib/tls`), so ping/dig/DNS are user-space code over sockets,
  not bespoke syscalls.
- **Kernel state is exposed through two synthetic filesystems** via the ordinary
  file API — no bespoke syscalls.
- **`/proc` (procfs.c) is flat, one fact per file.** Read-only info files:
  `cpuinfo meminfo uptime tasks date owofs storage pci journal usb`. A small
  number of **write control nodes** trigger a kernel action (e.g. `hostclip` for
  the VMware clipboard, `login`). System identity (incl. the kernel-authoritative
  machine arch) is `SYS_UNAME`/`uname(2)`, not a `/proc` file — the one info
  primitive that stays a syscall because a 32-bit user program can't read the
  kernel's arch from `/proc` any other way.
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
  `/sys` for ls/stat/read; syscall.c routes writes. A genuine *action* that
  returns nothing stays a syscall (e.g. `SYS_POWER`). The terminal captures a
  child's stdout via `SYS_SPAWN_CAP` (+ `read_nb`/`kill`).
- Each task carries a **cwd** (`task_t.cwd`); a parent sets a child's via
  `sched_set_cwd`, fork/`spawn_cap` inherit it, `SYS_CHDIR` sets the caller's, and
  path syscalls `join_cwd` relative names before hitting owofs.
- User programs are freestanding ELF32 (`user/apps/{gui,cli}/NAME.c`) linked at
  `0x40000000` (`user/user.ld`). Build: compile → `NAME.elf` → objcopy renames the
  blob symbols to `_binary_app_NAME_{start,end}` → `kernel/NAME_blob.o` → linked
  in. **Adding a program = two edits** (`NAME_blob.o` in `kernel/build.mk` OBJS +
  an `INSTALL_APP(NAME)` line in `kernel_main`); a generic `%.elf` rule covers
  plain apps, so a *third* edit (a link rule in `user/apps/build.mk`) is needed
  only if the program pulls in extra libs. No shell wiring — `run()`'s final
  `else` spawns any non-builtin as a `/bin` program.
- **Install/refresh:** owofs (`disk.img`) persists installed `/bin` programs.
  `install_blob()` reinstalls when absent **or its on-disk size differs from the
  built-in blob**, so a rebuild replaces the persisted binary next boot — no
  `make distclean` needed (except the rare same-byte-size behavior change).
  `/etc/passwd`+`/etc/group` stay install-if-absent so accounts persist.

## Storage: block dispatch & filesystem (blk.c + fs.c + drivers)

- owofs (fs.c) is a 512-byte-sector filesystem with a directory tree (mkdir +
  subdirs). `blk.c` picks **one** backing device at boot — first present of
  IDE → AHCI → NVMe (`blk_backend()` names it) — and routes fs.c's sector I/O to
  it via `blk_dev_io`.
- **Auto-format only a blank IDE disk** (`blk_is_ide`); fs_init **never**
  auto-formats AHCI/NVMe (the user's real OS drive).
- **tmpfs fallback:** with no usable owofs disk, fs_init calls `blk_use_ram()` → a
  volatile RAM disk above the identity map (`blk_backend()` → `"tmpfs"`).
- **Removable volumes mount under `/media/<name>`** via a multi-instance mount
  table (fsmount.h): several at once, including duplicates. Each driver
  (fat/hfs/ext/iso9660/ntfs) is a `struct fs_ops` vtable over an opaque per-mount
  instance; fat.c is read+write, the rest read-only. `mount_for()` routes a path
  by longest-prefix; `/media` and `/dev` are synthetic. **Nothing auto-mounts** —
  `mount <dev> [point]` / `umount <point>` or the Files app; fs.c owns mounting.
- `blk_dev_list()` reports *present* removable devices (candidate order CD → USB →
  IDE → SATA → NVMe → SD, purely each driver's `_present()`, no sector I/O).
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
  on the secondary IDE master. **iso9660.c** is read-only ISO 9660 / Joliet on
  atapi.c (`atapi_read_sector`), so blk.c mounts the CD directly.

## Network stack (nic.c → net.c → tcp.c, with crypto.c; TLS in user/lib/tls)

- Kernel-side stack; every client is user space over `net_sock_*` — `ping`/`dig`,
  `ssh`/`sshd`, and the network daemons (`dhcpcd`/`ntpd`/`mdnsd`/`smbd`). The
  richest is **curl** (`user/apps/cli/curl.c`): HTTP/1.1, HTTP/2 (`--http2`,
  `user/lib/curl/h2.c`), HTTP/3 (`--http3`, `user/lib/curl/{quic,h3}.c`), all in
  user space (no Berkeley sockets; doesn't advertise gzip).
- **nic.c — link-layer dispatch.** Each driver exports `struct nic_ops`;
  `nic_init` probes `{ igb, e1000, rtl8139 }` and keeps the first present.
  `nic_set()` installs a hot-plugged driver (RNDIS) at runtime.
  - rtl8139.c — the QEMU default.
  - e1000.c — Intel **I217–I219** (PCH e1000e/ich8lan), *not* I210/I211: MMIO
    register file (UC-), legacy RX/TX rings, RX IRQ. `make run NIC=e1000`.
  - igb.c — Intel **I210/I211/82576/82580/I350**: a different MAC (per-queue
    descriptor windows, advanced descriptors, per-queue ENABLE handshake), RX
    drained off the timer tick (no RX IRQ). I211 (0x1539) is the real-HW target;
    QEMU's 82576 is the in-emulation proxy. `make run NIC=igb`.
- **net.c — IPv4:** ARP (cached), ICMP echo, UDP; the DNS resolver is user-space.
  DHCP and NTP are user-space daemons — `/bin/dhcpcd` autostarts the boot lease,
  `/bin/ntpd` on demand.
- **tcp.c** — active + passive open, several concurrent TCBs demuxed by 4-tuple; a
  SYN is accepted in the IRQ and handed to a daemon thread. In-order only, retx
  only SYN/SYN-ACK, no congestion control, no TIME_WAIT — fine over SLIRP.
- **user/lib/tls + user/lib/crypto** — TLS 1.3 (1.2 fallback), X25519 +
  AES-128-GCM + SHA-256/HMAC/HKDF, freestanding, in user space. (The kernel's own
  `crypto.c` serves only users.c password hashing.) **The server cert is not
  verified** — deliberate: resists passive eavesdropping, not active MITM.
- **RX runs in interrupt context**, so blocking client APIs (`tcp_read` …)
  `hlt`-spin while the NIC IRQ fills buffers, rather than touching the wire.
- **QUIC + HTTP/3 are entirely user space** (QUIC v1 over `SK_UDP`, own TLS 1.3 in
  CRYPTO frames + QPACK static-table). One connection at a time, cert not verified,
  loss-free assumed. Two **general** kernel enablers in net.c: the UDP socket
  buffers a *ring* of datagrams (`SOCK_DGRING`), and `net_udp_send`'s cap is a full
  MTU (QUIC's Initial must be ≥1200 bytes).

## Services & init (user-space daemons via /etc/inittab)

Daemons are ordinary user-space `/bin` programs; the kernel has no service registry.

- **`/bin/init` (PID 1, uid 0)** is spawned by `boot_bringup` after `sti`. It reads
  `/etc/inittab` (one `/bin` daemon per line; blank/`#` ignored — seeded
  `compositor`, `dhcpcd`, `mdnsd`, `sshd`) and launches each detached. init runs as
  uid 0 so its children get the privileges `dhcpcd`'s raw socket and `sshd`'s
  spawn-as-user need.
- **Each daemon owns its own loop** over `net_sock_*`. Stream servers (`sshd`,
  `smbd`) loop `net_listen(port) → net_accept → serve → close`; the kernel runs no
  UDP-in-IRQ service hooks.
- **mDNS advertisements** live in `/etc/mdns.conf` (one `type port [txt...]` per
  line; seeds `_ssh._tcp 22`, `_smb._tcp 445`), read by `/bin/mdnsd`.
  `/etc/inittab` and `/etc/mdns.conf` are both install-if-absent, so user edits
  persist across reboots.
- Testing an inbound daemon needs a `hostfwd` (SLIRP blocks inbound); `make
  run`/`debug` bake in `hostfwd=tcp::1445-:445` (smbd) + `hostfwd=tcp::2222-:22`
  (sshd). Guest IP is 10.0.2.15.

## Subsystem pointers (non-obvious constraints; internals + file layout in the source)

- **SMB (`smbd`)** — read-only SMB2 (dialect 2.0.2) exposing owofs as share
  `owofs`, one client at a time. **Auth is cosmetic** (any credentials → guest).
  `smbclient -L` won't work (no IPC$ srvsvc pipe) — connect to the share directly.
- **SSH (`ssh` client + `sshd`)** — curve25519-sha256 KEX, ed25519 host keys,
  `aes128-gcm@openssh.com`. `sshd` autostarts and runs a shell per session as the
  authenticated user; the deep KEX crypto on-stack is why `KSTACK_SIZE` is 64 KiB.
- **BitTorrent (user/lib/torrent, `/bin/torrent`)** — TCP-only over `tcp_connect`.
- **SQLite (user/lib/sqlite)** — reader/writer for the real SQLite3 format,
  in-memory (whole-file `sql_open`/`sql_save`). Table b-trees + overflow-page
  chains (records larger than a 4 KiB page spill onto them, written and read) +
  NULL/int/float/TEXT/BLOB; **no index b-trees or WAL**, and reads only files
  with zero reserved bytes per page. First user is the browser cookie+HSTS jar
  (`browser/jar.c`, `/home/root/cookies.db`). Host round-trip test:
  `scripts/sqlite_test.c` (cross-checked against the host `sqlite3` CLI).
- **HTTP headers (user/lib/curl/headers.c)** — response headers parsed once into a
  case-insensitive k/v object (repeats kept in order), shared by curl + browser
  (which also honors `X-Content-Type-Options: nosniff`).
- **HTTP cache (user/lib/curl/httpcache + browser cache.c)** — the RFC 7234 policy
  (date parse, Cache-Control/Expires/Age/ETag/Last-Modified → storable/freshness/
  age) is a pure host-testable lib (`scripts/httpcache_test.c`); `cache.c` is the
  store (LRU, size-capped, spinlocked). Wired into `fetch.c` (all transports, incl.
  304 revalidation) and `resource.c`. **Bodies are one file per entry** under
  `/home/root/.cache/browser` (not SQLite — its 4 KiB page is too small for
  bodies), with an in-memory index. owofs allows one open write stream at a time,
  so writes/deletes **defer to `cache_flush()`** on the worker thread after a
  navigation; `cache_load()` restores at startup.
- **Notifications (user/lib/notify + notifyd + notify-send)** — resident `notifyd`
  listens on the `notify` IPC port (length-prefixed strings), keeps history, draws
  the Notification Center; a button can run a shell command on click. A second
  `notifyd` launch just asks the resident one to open its window.
- **Media (user/lib/media + player + browser `<audio>`/`<video>`)** — audio decoder
  (WAV/MPEG → 48 kHz/S16/stereo) + a **user-space software mixer** on a background
  thread, because the kernel exposes a single exclusive PCM stream; files decode
  incrementally. **Video** is `user/lib/media/video.o` (`$(VIDEO)` = video.o +
  `$(H264)` + `$(MP4)`): `media_video_*` decodes a local `.264`/`.mp4` to timed RGB
  frames (container sniffed by content, not extension). `/bin/player` renders it
  (window sized to the clip, blit paced by the wall clock); the browser `<video>`
  decodes on a per-page background thread and blits frames into the poster (double-
  buffered; the paint holds `blk_lock` and the swap is under it, so the front frame
  is read lock-free during draw). An MP4's AAC audio is not decoded → video plays
  silently; when a container carries WAV/MP3 audio the mixer is the master clock.
  Decode faults are user-space crashes only.
- **WebP (user/lib/webp + CLI)** — VP8L **lossless** only (encode + decode),
  in-memory; wired into imgview/browser. Lossy VP8 + animation are rejected
  (`WEBP_EUNSUP`).
- **JSON (user/lib/json + CLI)** — RFC 8259 parser/encoder → malloc'd value tree
  (numbers are IEEE-754 doubles). Pure libc + libmath, no engine coupling.
- **CORS (browser/cors.c)** — pure stateless policy (origin extraction, same-
  origin/same-site, preflight) for the browser's CORS + CORP guards; `resource.c`
  orchestrates. Registrable-domain check approximates the PSL with the last two
  dotted labels.
- **Brotli (user/lib/brotli)** — RFC 7932 decoder + encoder for the HTTP `br`
  coding; verified vs the system `brotli` CLI. The HTTP/1.1 path advertises `br`.
- **ACPI/power/battery (power.c + aml.c + battery.c)** — mostly real-HW.
  `power_off()` does ACPI S5 (finds the RSDP via legacy scan or the mb2 copy,
  walks XSDT/RSDT → FADT → DSDT). aml.c is a tiny interpreter scoped to the
  battery; **fails gracefully** (returns 0, never crashes). Keep its diagnostics.
- **USB host & tethering (xhci.c + usb.c)** — `lsusb` over UHCI + xHCI; `xhci_boot`
  keeps the host persistent. Goal: phone tethering via an RNDIS `nic_ops`.
  **Tethering does not work on real hardware** — the wired NIC is the real path.
- **Audio (drivers/audio: sb16.c + hda.c)** — `play <file.wav>` prefers HDA
  (`hda_present()`) else SB16. `hda_init` runs before `sti` (busy-waits); DMA
  buffers from `pmm_alloc_contig` must be `paging_identity_map`'d before first
  touch; volume/Fn keys run from the keyboard IRQ. Realtek codec quirks are real-HW.
- **Transport & codec libs:** `user/lib/netconn` (TCP-or-TLS wrapper),
  `user/lib/resolve` (hostname→IPv4 via DNS + mDNS), `user/lib/websocket`
  (RFC 6455), `user/lib/png` (streaming decoder on `inflate.c`), `user/lib/mp3`
  (vendored minimp3 for `/bin/play`), `user/lib/alac` (Apple Lossless, `/bin/alac`).
- **MPEG-1 video + MP4 (`user/lib/mpeg`, `user/lib/mp4`, `/bin/mpeg`)** — a
  from-scratch MPEG-1 video codec (ISO 11172-2): decoder (I/P/B, half-pel motion
  comp, DCT/IDCT, run/level VLCs) + I/P encoder (integer-pel full-search ME,
  constrained to in-frame MVs so output is portable to any decoder). Both round-
  trip through ffmpeg's mpeg1video (I-frames bit-exact); the VLC tables in
  `mpeg_tables.h` are generated verbatim from FFmpeg's `mpeg12data.c` and the IDCT
  basis is precomputed (no runtime `cos`). `user/lib/mp4` is a codec-agnostic
  ISO-BMFF demuxer (box tree → tracks + flat sample table, identifies H.264/H.265/
  AAC, extracts avcC SPS/PPS) — the substrate a video decoder consumes; it decodes
  nothing itself. Host tests: `scripts/{mpeg,mpeg_enc,mp4}_test.c` (verified against
  ffmpeg-generated vectors + a hand-built MP4).
- **H.264 / AVC (`user/lib/h264`, `/bin/h264`)** — a from-scratch Constrained
  Baseline / Baseline **decoder** (CAVLC): NAL/RBSP de-escaping, SPS/PPS/slice
  parsing, I + P slices, intra (4x4/16x16/chroma) + inter prediction (quarter-pel
  6-tap luma, 1/8-pel chroma, all partitions + multi-reference), the 4x4 integer
  transform with luma/chroma DC Hadamard, and the in-loop deblocking filter. Two
  front ends over one core: `h264_decoder_open` for an Annex-B elementary stream,
  and `h264_decoder_open_avcc` + `h264_decode_au` for an MP4 track's avcC +
  length-prefixed access units (via `user/lib/mp4`). Split across
  `nal/cavlc/transform/intra/inter/deblock/slice/h264.c`; the CAVLC + deblock VLC/
  threshold tables are copied verbatim from FFmpeg's `h264_cavlc.c`/
  `h264_loopfilter.c`. **Bit-exact vs ffmpeg/libx264** for the intra, inter and
  uniform-QP deblock paths; deblocking leaves ≤1 LSB on some variable-QP streams
  (imperceptible). Not decoded (→ `H264_EUNSUP`): CABAC, B slices, the 8x8
  transform + scaling lists (High profile), interlaced/field, FMO/ASO. 4:2:0 only.
  Host test: `scripts/h264_test.c` (PSNR vs ffmpeg raw YUV).
- **Cast / Chromecast (`user/lib/cast`, `/bin/cast`, browser Remote Playback)** —
  a Cast *sender*: mDNS discovery → a TLS:8009 channel of length-prefixed
  CastMessage protobufs ($(PROTOBUF)) carrying JSON payloads ($(JSON)), driving
  the Default Media Receiver (`CC1AD845`). A session owns a **reader + pinger
  thread**: the kernel's blocking TCP recv self-closes after ~5 s idle and a
  receiver's heartbeat sits on that edge, so the pinger PINGs every 2 s to keep
  inbound traffic flowing (TLS has no non-blocking read). Status is polled, never
  callback. The browser's `HTMLMediaElement.remote` (W3C Remote Playback API,
  js.c) is a thin consumer, delivering availability + connect/disconnect from the
  worker via `js_cast_pump`. **Real-hardware-only** (no Cast device under SLIRP);
  `cast discover` degrades to "none" there. A CLI tool only renders inline in
  `/bin/term` if it's in that file's `is_cli()` list.
