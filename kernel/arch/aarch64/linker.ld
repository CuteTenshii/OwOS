/* linker.ld: AArch64 kernel image, linked at 0 as a PIE: boot.S adds the load address to every R_AARCH64_RELATIVE
 * entry between __rela_start and __rela_end, so one image runs wherever the loader puts it (0x40080000 on QEMU virt,
 * 0x80080000 on Tegra X1). */
ENTRY(_start)

SECTIONS
{
  . = 0;
  __kernel_start = .;

  .text : {
    *(.text.boot)
    *(.text .text.*)
  }

  .rodata : ALIGN(16) { *(.rodata .rodata.*) }
  .data.rel.ro : ALIGN(16) { *(.data.rel.ro .data.rel.ro.*) }
  .got : ALIGN(8) { *(.got .got.plt) }
  .dynamic : ALIGN(8) { *(.dynamic) }
  .dynsym : ALIGN(8) { *(.dynsym) }
  .dynstr : { *(.dynstr) }
  .hash : ALIGN(8) { *(.hash .gnu.hash) }
  .data   : ALIGN(16) { *(.data .data.*) }

  .rela.dyn : ALIGN(8) {
    __rela_start = .;
    *(.rela .rela.*)
    __rela_end = .;
  }

  .bss : ALIGN(16) {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  }

  __kernel_end = .;
  kernel_end = .;
  __image_size = __kernel_end - __kernel_start;

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) }
}
