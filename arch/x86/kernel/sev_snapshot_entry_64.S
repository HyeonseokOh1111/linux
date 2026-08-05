/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SEV-SNP snapshot restore entry glue.
 *
 * Keep this as raw assembly rather than C-callable helper code.  These paths
 * resume into entry_64.S return code with a live pt_regs frame and no extra
 * call frame.
 */

#include <linux/linkage.h>
#include <asm/segment.h>
#include <asm/page_types.h>
#include <asm/ptrace-abi.h>
#include <asm/unwind_hints.h>

.code64
.section .entry.text, "ax"

	.extern restore_regs_and_return_to_kernel
	.extern sev_snapshot_iret_from_ptregs
	.extern sev_snapshot_sysret_from_ptregs
	.extern swapgs_restore_regs_and_return_to_usermode
	.extern sev_snapshot_restore_from_mailbox

SYM_CODE_START(sev_snapshot_resume_from_ptregs)
	movq	%rdi, %rsp
	UNWIND_HINT_REGS

	testb	$3, CS(%rsp)
	jz	.Lsev_snapshot_restore_to_kernel
	jmp	sev_snapshot_iret_from_ptregs
.Lsev_snapshot_restore_to_kernel:
	jmp	restore_regs_and_return_to_kernel
SYM_CODE_END(sev_snapshot_resume_from_ptregs)

SYM_CODE_START(sev_snapshot_resume_kernel_thread_from_ptregs)
	movq	%rdi, %r10

	testb	$3, CS(%r10)
	jz	.Lsev_snapshot_resume_kernel_thread
	ud2

.Lsev_snapshot_resume_kernel_thread:
	movq	RSP(%r10), %rsp
	movq	RIP(%r10), %r11
	pushq	%r11

	movq	EFLAGS(%r10), %r11
	pushq	%r11
	popfq

	movq	R15(%r10), %r15
	movq	R14(%r10), %r14
	movq	R13(%r10), %r13
	movq	R12(%r10), %r12
	movq	RBP(%r10), %rbp
	movq	RBX(%r10), %rbx
	movq	R9(%r10), %r9
	movq	R8(%r10), %r8
	movq	RCX(%r10), %rcx
	movq	RDX(%r10), %rdx
	movq	RSI(%r10), %rsi
	movq	RDI(%r10), %rdi
	movq	RAX(%r10), %rax
	movq	R11(%r10), %r11
	movq	R10(%r10), %r10
	ret
SYM_CODE_END(sev_snapshot_resume_kernel_thread_from_ptregs)

SYM_CODE_START(sev_snapshot_restore_entry_early)
	UNWIND_HINT_END_OF_STACK

	cli
	cld

	movq	%rdi, %r12
	movq	%rsi, %r13

	lgdt	.Lsev_snapshot_restore_early_gdt_desc(%rip)

	movw	$__KERNEL_DS, %ax
	movw	%ax, %ss
	movw	%ax, %ds
	movw	%ax, %es

	pushq	$__KERNEL_CS
	leaq	.Lsev_snapshot_restore_entry_cs_ready(%rip), %rax
	pushq	%rax
	lretq

.Lsev_snapshot_restore_entry_cs_ready:
	/* Switch to the saved Linux CR3 and use space below pt_regs as C stack. */
	movq	%r14, %rax
	andq	$~0xfff, %rax
	movq	%rax, %cr3

	movq	%r13, %rax
	subq	$0x800, %rax
	andq	$~0xf, %rax
	subq	$8, %rax
	movq	%rax, %rsp
	movq	$0, (%rsp)

	movq	%r12, %rdi
	movq	%r10, %rsi
	jmp	sev_snapshot_restore_from_mailbox

	ud2
SYM_CODE_END(sev_snapshot_restore_entry_early)

	.balign 8
.Lsev_snapshot_restore_early_gdt:
	.quad	0x0000000000000000
	.quad	0x00cf9b000000ffff
	.quad	0x00af9b000000ffff
	.quad	0x00cf93000000ffff
.Lsev_snapshot_restore_early_gdt_end:
.Lsev_snapshot_restore_early_gdt_desc:
	.word	.Lsev_snapshot_restore_early_gdt_end - .Lsev_snapshot_restore_early_gdt - 1
	.quad	.Lsev_snapshot_restore_early_gdt
