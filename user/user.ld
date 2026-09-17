/* user.ld — link a user program as a flat binary at the user load address.
 * .text.start (the _start stub) goes first so the entry sits at USER_BASE.
 * Must match USER_BASE in kernel/loader.h: a per-process 1 GiB window backed by
 * pmm high memory, so programs link at 0x40000000 (was 0x280000). */

ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5); /* read + execute */
	data PT_LOAD FLAGS(6); /* read + write   */
}

SECTIONS
{
	. = 0x40000000;

	.text : { *(.text.start) *(.text*) } :text
	.rodata : { *(.rodata*) } :text
	. = ALIGN(CONSTANT (MAXPAGESIZE));
	.data : { *(.data*) } :data
	.bss : { *(.bss*) *(COMMON) } :data

	/DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
