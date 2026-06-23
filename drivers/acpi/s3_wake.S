/* s3_wake.S — ACPI S3 (suspend-to-RAM) suspend/resume plumbing.
 *
 * Two halves:
 *
 *   s3_enter() — called by power_sleep() with interrupts on. It snapshots the
 *   CPU state the firmware will NOT preserve across S3 (control regs, GDTR/IDTR,
 *   callee-saved regs, esp/ebp, eflags, and a return EIP), then writes
 *   SLP_TYP|SLP_EN to the PM1 control register(s). The chipset cuts power to the
 *   CPU; RAM is retained.
 *
 *   s3_trampoline_start..end — a tiny real-mode blob copied to physical 0x8000.
 *   On wake the firmware re-enters the machine in REAL MODE at the FACS waking
 *   vector (which power_sleep points here). The blob enables A20, switches to
 *   32-bit protected mode with a private flat GDT, and jumps to s3_resume_pm.
 *
 *   s3_resume_pm — the 32-bit landing pad (ordinary kernel text). Paging is off
 *   here, so linear == physical and the kernel (linked/loaded at 1 MiB, identity
 *   mapped) runs at its link address. It restores CR3/CR4/CR0 (re-enabling
 *   paging), reloads the real GDT/IDT, restores the saved registers and stack,
 *   then jumps back into s3_enter so it returns to power_sleep as if it had
 *   merely slept.
 *
 * The trampoline must be position-independent: it is assembled into the kernel
 * at some high address but executes from 0x8000, so every internal reference is
 * computed as TRAMP + (label - s3_trampoline_start), and data is reached with
 * DS = 0 (absolute low addressing), never via %cs (whose value depends on how
 * the firmware forms the real-mode CS:IP from the vector). */

#ifndef __x86_64__
	.set TRAMP, 0x8000          /* physical load address of the trampoline */

/* ---- saved CPU state -----------------------------------------------------
 * This MUST live in low conventional RAM (< 0xA0000): QEMU/SeaBIOS preserve
 * that across an S3 cycle, but the kernel .bss (~4 MiB, extended memory) is NOT
 * preserved — it reads back as zero on resume, which loaded CR0=0, dropped us
 * to real mode, and made `ltr` an invalid opcode. So we park the saved state at
 * a fixed low address (0x9000), just above the wake trampoline at 0x8000 (which
 * we copy there and which likewise survives). s3_enter writes it after the
 * trampoline copy; s3_resume_pm reads it. Reachable with paging on or off. */
	.set SAVE,         0x9000
	.set saved_eflags, SAVE + 0
	.set saved_ebx,    SAVE + 4
	.set saved_esi,    SAVE + 8
	.set saved_edi,    SAVE + 12
	.set saved_ebp,    SAVE + 16
	.set saved_esp,    SAVE + 20
	.set saved_eip,    SAVE + 24
	.set saved_cr0,    SAVE + 28
	.set saved_cr3,    SAVE + 32
	.set saved_cr4,    SAVE + 36
	.set saved_gdtr,   SAVE + 40     /* 6 bytes (limit+base) */
	.set saved_idtr,   SAVE + 48     /* 6 bytes */

	.text

/* void s3_enter(uint32_t pm1a_port, uint32_t pm1b_port,
 *               uint32_t slp_a_value, uint32_t slp_b_value)
 * cdecl: args at 4/8/12/16(%esp) after entry. */
	.code32
	.global s3_enter
s3_enter:
	/* Capture EFLAGS *before* masking interrupts: the caller (the WM thread)
	 * runs with IF=1, and resuming with IF=0 would halt the thread forever
	 * the next time it cooperatively yields (see CLAUDE.md). */
	pushf
	pop %eax
	mov %eax, saved_eflags
	cli

	mov %ebx, saved_ebx
	mov %esi, saved_esi
	mov %edi, saved_edi
	mov %ebp, saved_ebp

	mov %cr0, %eax
	mov %eax, saved_cr0
	mov %cr3, %eax
	mov %eax, saved_cr3
	mov %cr4, %eax
	mov %eax, saved_cr4

	sgdt saved_gdtr
	sidt saved_idtr

	movl $s3_back, saved_eip
	mov %esp, saved_esp          /* points at the return address; args above */

	/* Write the sleep command: outw(SLP_TYPa|SLP_EN) to PM1a_CNT, and to
	 * PM1b_CNT too when the FADT supplies a second block. */
	mov 4(%esp), %edx            /* pm1a_port */
	mov 12(%esp), %eax           /* slp_a_value */
	out %ax, %dx

	mov 8(%esp), %edx            /* pm1b_port (0 if none) */
	test %edx, %edx
	jz s3_no_pm1b
	mov 16(%esp), %eax           /* slp_b_value */
	out %ax, %dx
s3_no_pm1b:
	/* If S3 engaged, the CPU is already powered down and we never run these.
	 * If it did NOT (S3 unsupported / disabled), don't hang forever: idle a
	 * bounded number of ticks, then fall through and return "slept = no". */
	mov $300, %ecx
s3_idle:
	sti
	hlt
	cli
	dec %ecx
	jnz s3_idle

s3_back:
	/* Resume rejoins here (via s3_resume_pm) with the stack restored. */
	ret

/* ---- 32-bit resume landing pad (entered from the trampoline) ------------- */
	.code32
	.global s3_resume_pm
s3_resume_pm:
	/* Paging is OFF (linear == physical), flat 32-bit segments are loaded
	 * from the trampoline's GDT, and A20 is on. Bring the kernel address
	 * space back, then reload the real descriptor tables. */
	mov saved_cr3, %eax
	mov %eax, %cr3
	mov saved_cr4, %eax
	mov %eax, %cr4
	mov saved_cr0, %eax
	mov %eax, %cr0               /* re-enables PG; mapping is identical here */

	lgdt saved_gdtr
	lidt saved_idtr
	/* CS still holds the flat ring-0 code descriptor cached from the trampoline
	 * GDT (identical to the kernel's 0x08), so an explicit CS reload isn't
	 * required. Reload the data segments from the restored GDT. */
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %ss
	mov %ax, %fs
	mov %ax, %gs

	/* Reload the task register. S3 clears TR, and the first interrupt taken
	 * while a ring-3 task is running reads TSS.esp0 through TR — a null TR
	 * triple-faults. The boot-time ltr left the TSS descriptor's busy bit set
	 * in the (RAM-resident) GDT, so clear it before re-loading or ltr #GPs.
	 * (Keep the GDT base in %eax untouched between the load and the andb.) */
	mov saved_gdtr+2, %eax        /* GDT linear base (sgdt: limit@0, base@2) */
	andb $0xFD, 0x2D(%eax)        /* TSS desc 0x28, access byte (+5): clr busy */
	mov $0x28, %ax
	ltr %ax

	mov saved_esp, %esp
	mov saved_ebp, %ebp
	mov saved_ebx, %ebx
	mov saved_esi, %esi
	mov saved_edi, %edi

	/* Restore EFLAGS last (re-enabling IF only now that the IDT and a valid
	 * kernel stack are in place), then rejoin s3_enter and return. */
	push saved_eflags
	popf
	jmp *saved_eip

/* ---- real-mode wake trampoline (copied verbatim to physical 0x8000) ------ */
	.code16
	.global s3_trampoline_start
	.global s3_trampoline_end
s3_trampoline_start:
	cli
	cld
	xor %ax, %ax
	mov %ax, %ds                 /* DS=0: reach our data by absolute low addr */

	/* Enable A20 (fast gate, port 0x92) so the >1 MiB kernel is reachable. */
	in $0x92, %al
	test $0x02, %al
	jnz s3_a20_ok
	or $0x02, %al
	and $0xFE, %al               /* keep bit0 (fast reset) clear */
	out %al, $0x92
s3_a20_ok:
	lgdtl TRAMP + (tramp_gdtr - s3_trampoline_start)

	mov %cr0, %eax
	or $0x1, %eax
	mov %eax, %cr0               /* enter protected mode (paging still off) */

	/* Far jump flushes the prefetch queue and loads CS = flat code selector.
	 * Target is the absolute physical address of pm32 within the blob. */
	ljmpl $0x08, $(TRAMP + (pm32 - s3_trampoline_start))

	.code32
pm32:
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %ss
	mov %ax, %fs
	mov %ax, %gs
	/* Hand off to the kernel resume pad (high memory; reachable with paging
	 * off because the kernel runs at its physical link address). */
	mov $s3_resume_pm, %eax
	jmp *%eax

	.align 8
tramp_gdt:
	.quad 0x0000000000000000     /* null */
	.quad 0x00CF9A000000FFFF     /* 0x08: 4 GiB flat code, 32-bit */
	.quad 0x00CF92000000FFFF     /* 0x10: 4 GiB flat data, 32-bit */
tramp_gdtr:
	.word (tramp_gdtr - tramp_gdt - 1)
	.long (TRAMP + (tramp_gdt - s3_trampoline_start))
s3_trampoline_end:

#else
/* ---- x86_64: S3 suspend write only ---------------------------------------
 * S3 (suspend-to-RAM) is real-hardware-only, and resuming on x86_64 means the
 * firmware re-enters in real mode and we would have to rebuild long mode from a
 * relocated trampoline — out of scope here. s3_enter still issues the actual
 * sleep command (plain port writes, so QEMU/chipsets that honor S3 do power
 * down), but the register-save/resume path is omitted; the symbols below exist
 * only so power.c links. Resume is not supported in this build. */
	.text
	.code64
	.global s3_enter
/* void s3_enter(uint32_t pm1a_port, uint32_t pm1b_port,
 *               uint32_t slp_a_value, uint32_t slp_b_value)  — SysV: rdi,rsi,rdx,rcx */
s3_enter:
	cli
	mov %edx, %eax               /* slp_a_value */
	mov %edi, %edx               /* pm1a_port   */
	out %ax, %dx
	test %esi, %esi              /* pm1b_port (0 if none) */
	jz 1f
	mov %ecx, %eax               /* slp_b_value */
	mov %esi, %edx               /* pm1b_port   */
	out %ax, %dx
1:	mov $300, %ecx               /* bounded idle if S3 didn't engage */
2:	sti
	hlt
	cli
	dec %ecx
	jnz 2b
	sti                          /* restore IF before returning (see CLAUDE.md) */
	ret
	.size s3_enter, . - s3_enter

	.global s3_resume_pm
s3_resume_pm:                        /* resume unsupported on x86_64 */
	hlt
	jmp s3_resume_pm

	.code16
	.global s3_trampoline_start
	.global s3_trampoline_end
s3_trampoline_start:
	cli
	hlt
s3_trampoline_end:
#endif
