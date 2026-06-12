/* user.ld — link a user program as a flat binary at the user load address.
 * .text.start (the _start stub) goes first so the entry sits at USER_BASE.
 * Must match USER_BASE in kernel/loader.h: a per-process 1 GiB window backed by
 * pmm high memory, so programs link at 0x40000000 (was 0x280000). */

ENTRY(_start)

SECTIONS
{
	. = 0x40000000;

	.text : { *(.text.start) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }

	/DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
