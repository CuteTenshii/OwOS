/* user.ld — link a user program as a flat binary at the user load address.
 * .text.start (the _start stub) goes first so the entry sits at 0x280000.
 * Must match USER_BASE in kernel/loader.h: the kernel image + BSS now reaches
 * ~2.1 MiB, so the per-process window starts at 2.5 MiB instead of 2 MiB. */

ENTRY(_start)

SECTIONS
{
	. = 0x280000;

	.text : { *(.text.start) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }

	/DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
