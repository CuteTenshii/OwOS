/* linker64.ld — physical memory layout for the 64-bit (x86_64) OwOS kernel.
 *
 * Identical layout to linker.ld (the 32-bit script): the kernel is loaded at
 * 1 MiB and stays identity-mapped in low physical memory, so the only real
 * difference is the output format the -m64 driver selects. The .multiboot
 * section is forced to the front so the bootloader finds the Multiboot header
 * within the first 8 KiB of the file (the long-mode transition happens after
 * the loader hands off in 32-bit protected mode — see boot.S).
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
	. = 1M;
	kernel_start = .;

	.text BLOCK(4K) : ALIGN(4K)
	{
		*(.multiboot)
		*(.text)
	}

	.rodata BLOCK(4K) : ALIGN(4K)
	{
		*(.rodata)
	}

	.data BLOCK(4K) : ALIGN(4K)
	{
		*(.data)
	}

	.bss BLOCK(4K) : ALIGN(4K)
	{
		*(COMMON)
		*(.bss)
	}

	kernel_end = .;
}
