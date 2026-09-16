# Repository Guidelines

## Project Structure

OwOS is a freestanding operating system written in C, with GAS assembly for
boot and low-level CPU code.

- `boot/` contains Multiboot startup code and early boot support.
- `kernel/` contains kernel subsystems, including memory, processes, filesystems,
  networking, graphics, and system calls.
- `drivers/` contains hardware drivers grouped by device area.
- `user/lib/` contains user-space libraries, and `user/apps/` contains programs
  embedded into the kernel image.
- `scripts/` contains host-side unit tests and the scripted QEMU test harness.
- `fonts/`, `hostinc/`, `tools/`, linker scripts, and `*.img` files provide assets.

## Build, Run, and Test Commands

Use the repository root as your working directory.

```sh
make                         # Build the default 32-bit owos.bin
make run                     # Boot the kernel in graphical QEMU
make debug                   # Boot headless with serial output in the terminal
make ARCH=x86_64             # Build the 64-bit kernel image
make iso                     # Build a GRUB BIOS+UEFI ISO (Linux)
make clean                   # Remove build artifacts, keeping disk images
```

For integration checks, first build the kernel, then run for example:
`scripts/qemu-test.sh -- "ls -l /home/root"`. Host unit tests are individual C
programs in `scripts/`; each file documents its compile and execution command in
its opening comment. No aggregate test or lint target exists.

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
