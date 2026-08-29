/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SEV-SNP snapshot restore entry glue.
 *
 * Restore the fixed kernel continuation captured by snapshot_enter or an AP
 * capture thread.  The saved flags have interrupts disabled; the C restore
 * label performs the matching local_irq_restore().
 */

#include <linux/linkage.h>
#include <asm/asm-offsets.h>

.code64
.section .entry.text, "ax"

SYM_CODE_START(sev_snapshot_resume_kernel_continuation)
	movq	%rdi, %r10

	movq	sev_snapshot_cont_sp(%r10), %rsp
	movq	sev_snapshot_cont_ip(%r10), %r11
	pushq	%r11

	movq	sev_snapshot_cont_flags(%r10), %r11
	pushq	%r11
	popfq

	movq	sev_snapshot_cont_r15(%r10), %r15
	movq	sev_snapshot_cont_r14(%r10), %r14
	movq	sev_snapshot_cont_r13(%r10), %r13
	movq	sev_snapshot_cont_r12(%r10), %r12
	movq	sev_snapshot_cont_bp(%r10), %rbp
	movq	sev_snapshot_cont_bx(%r10), %rbx
	movq	sev_snapshot_cont_r9(%r10), %r9
	movq	sev_snapshot_cont_r8(%r10), %r8
	movq	sev_snapshot_cont_cx(%r10), %rcx
	movq	sev_snapshot_cont_dx(%r10), %rdx
	movq	sev_snapshot_cont_si(%r10), %rsi
	movq	sev_snapshot_cont_di(%r10), %rdi
	movq	sev_snapshot_cont_ax(%r10), %rax
	movq	sev_snapshot_cont_r11(%r10), %r11
	movq	sev_snapshot_cont_r10(%r10), %r10
	ret
SYM_CODE_END(sev_snapshot_resume_kernel_continuation)
