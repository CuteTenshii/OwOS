/* linker.ld: AArch64 kernel image for the QEMU virt machine (RAM at 0x40000000). */
ENTRY(_start)

SECTIONS
{
  . = 0x40080000;
  __kernel_start = .;

  .text : {
    *(.text.boot)
    *(.text .text.*)
  }

  .rodata : ALIGN(16) { *(.rodata .rodata.*) }
  .data   : ALIGN(16) { *(.data .data.*) }

  .bss : ALIGN(16) {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  }

  __kernel_end = .;

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
