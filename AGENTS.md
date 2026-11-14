# Repository Guidelines

## Project Structure

OwOS is a freestanding operating system written in C, with GAS assembly for
boot and low-level CPU code.

- `kernel/` contains kernel subsystems, including memory, processes, filesystems,
  networking, graphics, and system calls.
- `kernel/arch/x86/` holds the x86 CPU layer shared by i386 and x86_64: Multiboot boot code, linker scripts,
  GDT/IDT and entry stubs, context switch, SMP bring-up, paging, and the PIT, CMOS RTC and COM port. Files that
  differ by word size come as `_32`/`_64` pairs, and `kernel/build.mk` picks the half matching `ARCH`.
  `kernel/arch/aarch64/` is the AArch64 layer under the same shared kernel and user programs, with its own
  `build.mk`; `kernel/arch/arm/` is a separate 32-bit bring-up kernel.
- `drivers/` contains hardware drivers grouped by device area.
- `user/lib/` contains user-space libraries, and `user/apps/<name>/` contains one directory per program,
  embedded into the kernel image.
- `tests/` contains the host unit-test suites and their harness (`tests.mk`, `run-tests.sh`, the `owos.h`
  host shim); `scripts/` holds dev tools such as the scripted QEMU test harness.
- `fonts/`, `hostinc/`, `tools/`, and `*.img` files provide assets.

## Build, Run, and Test Commands

Use the repository root as your working directory.

```sh
make                         # Build the default 32-bit owos.bin
make run                     # Boot the kernel in graphical QEMU
make debug                   # Boot headless with serial output in the terminal
make ARCH=x86_64             # Build the 64-bit kernel image
make iso                     # Build a GRUB BIOS+UEFI ISO (Linux)
make ARCH=aarch64 check      # Build the AArch64 kernel, boot it on QEMU virt and run programs from its console
make ARCH=arm check          # Build the 32-bit ARMv7-A bring-up kernel and boot its self-test in QEMU (tests/arm)
make clean                   # Remove build artifacts, keeping disk images
```

For integration checks, first build the kernel, then run for example:
`scripts/qemu-test.sh -- "ls -l /home/root"`. Host unit tests are individual C
programs in `tests/`; each file documents its compile and execution command in
its opening comment. `make test` runs the deterministic host suite. No aggregate
lint target exists.

## Coding Style and Naming

Match nearby C and assembly code. Use tabs where the surrounding file does,
snake_case for functions and variables, uppercase names for macros, and concise
comments only where the code needs non-obvious context. Keep architecture-specific
  logic in the boot, kernel, or driver file. Add dependencies to the
appropriate `build.mk` instead of introducing recursive build scripts.

## Testing Guidelines

Exercise changed library logic with its host test, and boot changes with `make debug`
or `scripts/qemu-test.sh`. Include the command and relevant serial output
when a change affects runtime behavior. Tests should fail on the behavior they
cover, not merely compile successfully.

## Commits and Pull Requests

Recent history uses short Conventional Commit-style subjects such as `feat:`.
Use a concise imperative subject, for example `fix: preserve FAT directory names`.
Pull requests should explain the change, list validation commands, and
call out architecture, QEMU, disk-image, or hardware assumptions. Include logs
or screenshots when they make kernel or desktop behavior easier to review.

## Safety Notes

`make distclean` deletes generated disk images. The `make usb` target writes an
ISO to a removable device and can erase it. Confirm the target device before
using either operation.
