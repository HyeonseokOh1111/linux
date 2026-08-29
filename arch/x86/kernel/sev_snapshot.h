/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AMD SEV-SNP guest-triggered snapshot support.
 *
 * This header is intentionally local to arch/x86/kernel/.  The snapshot
 * mailbox/control layouts are part of the host/guest contract used by
 * the x86 snapshot path, but the implementation itself stays private to the
 * x86 SEV code.
 */

#ifndef __ARCH_X86_KERNEL_SEV_SNAPSHOT_H
#define __ARCH_X86_KERNEL_SEV_SNAPSHOT_H

#include <linux/types.h>
#include <linux/sizes.h>

#include <asm/bootparam.h>
#include <asm/desc_defs.h>
#include <asm/segment.h>
#include <asm/sev-common.h>

struct sev_snapshot_shared_extent {
	u64 gpa;
	u64 npages;
};

struct sev_snapshot_shared_extent_header {
	u32 shared_extent_count;
	u32 _alignment_padding;
};

static_assert(sizeof(struct sev_snapshot_shared_extent_header) == sizeof(u64));

#define SEV_SNAPSHOT_FIXED_MAILBOX_PA		0x00e00000ULL
#define SEV_SNAPSHOT_MAILBOX_SIZE		SZ_2M
#define SEV_SNAPSHOT_INPUT_PRIVATE_KEY_OFFSET	0x000ff000ULL
#define SEV_SNAPSHOT_INPUT_PRIVATE_KEY_LEN	32U
#define SEV_SNAPSHOT_LANDING_STACK_SCRATCH_SIZE	0x800UL
#define SEV_SNAPSHOT_LANDING_STACK_ALIGNMENT	16UL
#define SEV_SNAPSHOT_LANDING_STACK_SENTINEL_SIZE	8UL

struct sev_snapshot_kernel_continuation {
	u64 r15;
	u64 r14;
	u64 r13;
	u64 r12;
	u64 bp;
	u64 bx;
	u64 r11;
	u64 r10;
	u64 r9;
	u64 r8;
	u64 ax;
	u64 cx;
	u64 dx;
	u64 si;
	u64 di;
	u64 ip;
	u64 flags;
	u64 sp;
};

struct sev_snapshot_cpu_context {
	struct sev_snapshot_kernel_continuation continuation;
	u64 restore_mailbox_va;
	u64 fs_base;
	u64 kernel_gs_base;
	u64 user_gs_base;
	u64 cr0;
	u64 cr3;
	u64 cr4;
	u64 efer;
	u64 xcr0;
	u64 restore_gdt_page_va;
	u64 restore_gdt_page_pa;
	u64 restore_from_mailbox_rip;
	u64 hardirq_stack_ptr;
	u32 pkru;
	u8 gdt_copy[GDT_SIZE];
	u8 xsave[PAGE_SIZE];
};
#define SEV_SNAPSHOT_MIN_CPUS		1U
#define SEV_SNAPSHOT_MAX_CPUS		64U

struct sev_snapshot_restore_barrier {
	u32 arrived;
	u32 resume;
};

static_assert(sizeof(struct sev_snapshot_kernel_continuation) == 0x90);
static_assert(sizeof(struct sev_snapshot_cpu_context) == 0x1180);
static_assert(sizeof(struct sev_snapshot_restore_barrier) == 8);
static_assert(offsetof(struct sev_snapshot_kernel_continuation, ip) == 120);
static_assert(offsetof(struct sev_snapshot_kernel_continuation, flags) == 128);
static_assert(offsetof(struct sev_snapshot_kernel_continuation, sp) == 136);
static_assert(offsetof(struct sev_snapshot_cpu_context, restore_mailbox_va) == 144);
static_assert(offsetof(struct sev_snapshot_cpu_context, cr3) == 184);
static_assert(offsetof(struct sev_snapshot_cpu_context, restore_from_mailbox_rip) == 232);
static_assert(offsetof(struct sev_snapshot_cpu_context, hardirq_stack_ptr) == 240);
static_assert(offsetof(struct sev_snapshot_cpu_context, pkru) == 248);
static_assert(offsetof(struct sev_snapshot_cpu_context, gdt_copy) == 252);
static_assert(offsetof(struct sev_snapshot_cpu_context, xsave) == 380);

extern void __noreturn sev_snapshot_restore_from_mailbox(void *mailbox,
						 u64 restore_cpu_id);
extern void __noreturn sev_snapshot_resume_kernel_continuation(
	const struct sev_snapshot_kernel_continuation *continuation);

bool sev_snapshot_mailbox_available(void);

#endif /* __ARCH_X86_KERNEL_SEV_SNAPSHOT_H */
