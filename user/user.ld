/* user.ld: link a user program at USER_BASE (common/uabi.h), which the build passes in as __user_base.
 * .text.start (the _start stub) goes first so the entry sits at the base of the image. */

ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5); /* read + execute */
	data PT_LOAD FLAGS(6); /* read + write   */
	tls PT_TLS;            /* __thread template: .tdata + .tbss */
}

SECTIONS
{
	. = __user_base;

	.text : { *(.text.start) *(.text*) } :text
	__text_start = ADDR(.text); /* bounds for crash handlers telling return addresses from data */
	__text_end = ADDR(.text) + SIZEOF(.text);
	.rodata : {
		*(.rodata*)
		/* The TLS block's size and alignment as data: on AArch64 code reaches symbols PC-relative, so it cannot
		 * take the small absolute values __tls_memsz and __tls_align as addresses. */
		. = ALIGN(8);
		__tls_layout = .;
		QUAD(__tls_memsz)
		QUAD(__tls_align)
	} :text
	. = ALIGN(CONSTANT (MAXPAGESIZE));
	.data : { *(.data*) } :data
	.tdata : { *(.tdata .tdata.*) } :data :tls
	.tbss : { *(.tbss .tbss.*) *(.tcommon) } :data :tls
	.bss : { *(.bss*) *(COMMON) } :data

	/* The first thread's TLS block (see tls_init in lib.c); later threads carve theirs from their stack. Room for
	 * the variables, padding to their alignment and the 16 bytes either TLS variant puts beside them. */
	__tdata_start = ADDR(.tdata);
	__tdata_end = ADDR(.tdata) + SIZEOF(.tdata);
	__tls_memsz = ADDR(.tbss) + SIZEOF(.tbss) - ADDR(.tdata);
	__tls_align = MAX(MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)), 4);
	.tlsmain (NOLOAD) : { . = ALIGN(MAX(16, __tls_align)); __tls_main = .; . += __tls_memsz + __tls_align + 16; } :data

	/DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
