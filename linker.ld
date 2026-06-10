/* linker.ld — physical memory layout for the OwOS kernel image.
 *
 * The kernel is loaded at 1 MiB. Below that lies real-mode/BIOS junk and the
 * VGA buffer, so 1 MiB is the conventional first safe spot. The .multiboot
 * section is forced to the front so the bootloader can find the header within
 * the first 8 KiB of the file.
 */

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
