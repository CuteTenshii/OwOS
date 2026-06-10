/* user.ld — link a user program as a flat binary at the user load address.
 * .text.start (the _start stub) goes first so the entry sits at 0x200000. */

ENTRY(_start)

SECTIONS
{
	. = 0x200000;

	.text : { *(.text.start) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }

	/DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
