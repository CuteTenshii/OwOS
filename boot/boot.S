/* boot.S — Multiboot entry point for OwOS.
 *
 * QEMU's built-in loader reads the Multiboot header below and jumps to
 * _start in 32-bit protected mode with paging off. Our only jobs here are
 * to give C a valid stack and hand control to kernel_main.
 */

/* Multiboot v1 header constants. */
.set ALIGN,    1 << 0              /* align loaded modules on page boundaries */
.set MEMINFO,  1 << 1              /* request a memory map from the loader    */
.set VIDEO,    1 << 2              /* request a graphics mode + framebuffer    */
.set FLAGS,    ALIGN | MEMINFO | VIDEO
.set MAGIC,    0x1BADB002          /* lets the bootloader find the header     */
.set CHECKSUM, -(MAGIC + FLAGS)    /* MAGIC + FLAGS + CHECKSUM must be 0       */

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM
/* Address fields (offsets 12..31): only consulted when the AOUT_KLUDGE flag
 * (bit 16) is set, which it isn't — we're a plain ELF. But the VIDEO fields
 * below sit at a fixed offset 32, so these five longs must physically exist as
 * zero padding to push the video request to the right place. */
.long 0                            /* header_addr                              */
.long 0                            /* load_addr                                */
.long 0                            /* load_end_addr                            */
.long 0                            /* bss_end_addr                             */
.long 0                            /* entry_addr                               */
/* VIDEO fields (flag bit 2). mode_type 0 = linear graphics framebuffer. We ask
 * for NO specific geometry (all zero = "no preference") on purpose: a hardcoded
 * resolution makes GRUB fail with "no suitable video mode found" on panels that
 * don't advertise exactly that mode. With 0/0/0, GRUB picks any linear mode the
 * GPU supports and reports the real geometry back in the Multiboot info struct;
 * fb_adopt then handles whatever it gets (and ignores non-32bpp). QEMU's -kernel
 * loader ignores these fields entirely, so that path falls back to DISPI. */
.long 0                            /* mode_type: 0 = linear graphics           */
.long 0                            /* width:  0 = no preference                */
.long 0                            /* height: 0 = no preference                */
.long 32                           /* depth:  32bpp (our XRGB8888 primitives)  */

/* ---- Multiboot2 header --------------------------------------------------- *
 * GRUB's multiboot1 (`multiboot`) handoff is broken under UEFI: its relocator
 * page-faults during the long->protected-mode transition before our kernel even
 * runs. Multiboot2 (`multiboot2`) is the path GRUB does correctly under EFI, so
 * we carry BOTH headers: QEMU's -kernel loader uses the v1 header above; GRUB
 * uses this v2 one. Same entry (eax=magic, ebx=info); kernel_main tells them
 * apart by the magic and parses each info format. Must be 8-byte aligned. */
.set MB2_MAGIC, 0xE85250D6
.set MB2_ARCH,  0                  /* 0 = i386 (32-bit protected mode entry)   */
.align 8
mb2_start:
.long MB2_MAGIC
.long MB2_ARCH
.long mb2_end - mb2_start                                   /* header length   */
.long -(MB2_MAGIC + MB2_ARCH + (mb2_end - mb2_start))       /* checksum        */
/* Framebuffer-request tag (type 5). flags bit 0 = 1 (optional): a board with no
 * usable graphics mode still boots to text rather than failing the load. 0/0/0
 * geometry lets the loader pick whatever the GPU/GOP supports. */
.align 8
.short 5                           /* tag type: framebuffer                    */
.short 1                           /* flags: optional                          */
.long 20                           /* tag size                                 */
.long 0                            /* width:  no preference                    */
.long 0                            /* height: no preference                    */
.long 32                           /* depth:  32bpp (our XRGB8888 primitives)  */
/* End tag. */
.align 8
.short 0
.short 0
.long 8
mb2_end:

/* 16 KiB stack. The System V ABI wants the stack 16-byte aligned. */
.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:

#ifdef __x86_64__
/* ---- x86_64 long-mode entry ---------------------------------------------- *
 * The Multiboot loader still enters in 32-bit protected mode with paging off
 * (both header `arch` fields are 0 = i386). We build a temporary identity map
 * of the low 4 GiB with 2 MiB pages, enable PAE + long mode + paging, load a
 * GDT carrying a 64-bit code segment, and far-jump into 64-bit code, which sets
 * the stack and calls kernel_main(magic, mbi). The kernel's own paging.c later
 * installs the real address space; this map only needs to get us into C.
 *
 * Page tables: PML4 -> one PDPT -> four PDs (one per GiB), each PD full of 512
 * 2 MiB pages. All of the kernel's working set (image at 1 MiB, heap, the user
 * window at 1 GiB, WIN_FB at 3 GiB) lives under 4 GiB, so the 2 MiB identity map
 * covers it without a single 4 KiB leaf. */
.section .bss
.align 4096
boot_pml4: .skip 4096
boot_pdpt: .skip 4096
boot_pd:   .skip 4096 * 4          /* four PDs = 2048 entries = 4 GiB         */

.section .rodata
.align 16
boot_gdt:
	.quad 0x0000000000000000       /* null                                    */
	.quad 0x00AF9A000000FFFF       /* 0x08: 64-bit kernel code (L=1)           */
	.quad 0x00CF92000000FFFF       /* 0x10: kernel data                        */
boot_gdt_end:
boot_gdtr:
	.word boot_gdt_end - boot_gdt - 1
	.long boot_gdt                 /* 32-bit base: lgdt runs in protected mode */

.section .bss
.align 4
mb_magic: .skip 4
mb_info:  .skip 4

.section .text
.global _start
.type _start, @function
.code32
_start:
	cli
	mov $stack_top, %esp
	mov %eax, mb_magic             /* stash loader handoff before clobbering   */
	mov %ebx, mb_info

	/* PML4[0] = boot_pdpt | present|writable */
	mov $boot_pdpt, %eax
	or  $0x3, %eax
	mov %eax, boot_pml4

	/* PDPT[0..3] = boot_pd + i*4 KiB | present|writable */
	mov $boot_pd, %eax
	or  $0x3, %eax
	mov $boot_pdpt, %edi
	mov $4, %ecx
1:	mov %eax, (%edi)
	add $0x1000, %eax
	add $8, %edi
	loop 1b

	/* PD[k] = (k << 21) | present|writable|page-size, k = 0..2047 (4 GiB). The
	 * high dword stays 0: every mapped address is below 4 GiB. */
	mov $boot_pd, %edi
	xor %ecx, %ecx
2:	mov %ecx, %eax
	shl $21, %eax
	or  $0x83, %eax
	mov %eax, (%edi)
	movl $0, 4(%edi)
	add $8, %edi
	inc %ecx
	cmp $2048, %ecx
	jne 2b

	mov $boot_pml4, %eax
	mov %eax, %cr3

	mov %cr4, %eax
	or  $0x20, %eax                /* CR4.PAE                                  */
	mov %eax, %cr4

	mov $0xC0000080, %ecx          /* EFER MSR                                 */
	rdmsr
	or  $0x100, %eax               /* EFER.LME                                 */
	wrmsr

	mov %cr0, %eax
	or  $0x80000000, %eax          /* CR0.PG -> long mode active (compat)      */
	mov %eax, %cr0

	lgdt boot_gdtr
	ljmp $0x08, $long_mode_start   /* reload CS with the 64-bit selector       */

.code64
long_mode_start:
	mov $0x10, %ax                 /* flat data selectors                      */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %ss
	mov %ax, %fs
	mov %ax, %gs
	mov $stack_top, %rsp

	mov mb_magic, %edi             /* arg1: magic  (SysV: rdi)                 */
	mov mb_info,  %esi             /* arg2: mbi    (SysV: rsi); zero-extended  */
	xor %rbp, %rbp
	call kernel_main
.hang64:
	hlt
	jmp .hang64
.size _start, . - _start

#else
.section .text
.global _start
.type _start, @function
_start:
	cli                            /* no IDT yet: mask IRQs or the timer       */
	                               /* (IRQ0 -> vector 8) triple-faults us       */
	mov $stack_top, %esp           /* set up the stack                        */

	/* The bootloader leaves the multiboot magic in EAX and a pointer to the
	 * info structure in EBX. Pass them to kernel_main(magic, mbi). */
	push %ebx                      /* arg 2: multiboot info pointer           */
	push %eax                      /* arg 1: multiboot magic                  */
	call kernel_main               /* into C                                  */

	                               /* kernel_main returned: halt forever      */
.hang:
	hlt
	jmp .hang
.size _start, . - _start
#endif
