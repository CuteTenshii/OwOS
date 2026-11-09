/* user.ld — link a user program as a flat binary at the user load address.
 * .text.start (the _start stub) goes first so the entry sits at USER_BASE.
 * Must match USER_BASE in kernel/loader.h: a per-process 1 GiB window backed by
 * pmm high memory, so programs link at 0x40000000 (was 0x280000). */

ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5); /* read + execute */
	data PT_LOAD FLAGS(6); /* read + write   */
	tls PT_TLS;            /* __thread template: .tdata + .tbss */
}

SECTIONS
{
	. = 0x40000000;

	.text : { *(.text.start) *(.text*) } :text
	.rodata : { *(.rodata*) } :text
	. = ALIGN(CONSTANT (MAXPAGESIZE));
	.data : { *(.data*) } :data
	.tdata : { *(.tdata .tdata.*) } :data :tls
	.tbss : { *(.tbss .tbss.*) *(.tcommon) } :data :tls
	.bss : { *(.bss*) *(COMMON) } :data

	/* The first thread's TLS block (see tls_init in lib.c); later threads carve theirs from their stack */
	__tdata_start = ADDR(.tdata);
	__tdata_end = ADDR(.tdata) + SIZEOF(.tdata);
	__tls_memsz = ADDR(.tbss) + SIZEOF(.tbss) - ADDR(.tdata);
	__tls_align = MAX(MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)), 4);
	.tlsmain (NOLOAD) : { . = ALIGN(16); __tls_main = .; . += __tls_memsz + 32; } :data

	/DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
