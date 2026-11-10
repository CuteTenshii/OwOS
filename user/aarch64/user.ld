/* user.ld: AArch64 user programs, one page-aligned segment per permission class. */
ENTRY(_start)

PHDRS
{
  text PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  . = 0x100000000;
  .text : ALIGN(4096) { *(.text.start) *(.text .text.*) } :text
  .rodata : ALIGN(4096) { *(.rodata .rodata.*) } :rodata
  .data : ALIGN(4096) { *(.data .data.*) } :data
  .bss : ALIGN(4096) { *(.bss .bss.*) *(COMMON) } :data

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.*) }
}
