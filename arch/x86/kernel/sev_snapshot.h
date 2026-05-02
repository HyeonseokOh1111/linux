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

#include <asm/bootparam.h>
#include <asm/desc_defs.h>
#include <asm/ptrace.h>
#include <asm/segment.h>
#include <asm/sev-common.h>

struct sev_snapshot_shared_extent {
	u64 gpa;
	u64 npages;
};

struct sev_snapshot_cpu_context {
	u32 context_size;
	u32 valid_flags;
	struct pt_regs regs;
	u64 kernel_regs_ptr;
	u64 restore_stub_rip;
	u64 restore_mailbox_va;
	u16 ds;
	u16 es;
	u16 fs;
	u16 gs;
	u16 ldt;
	u16 tss;
	u64 fs_base;
	u64 kernel_gs_base;
	u64 user_gs_base;
	u64 cr0;
	u64 cr2;
	u64 cr3;
	u64 cr4;
	u64 efer;
	struct desc_ptr gdt_desc;
	struct desc_ptr idt;
	u64 tr;
	u64 xcr0;
	u64 xstate_bv;
	u32 xsave_len;
	u32 xsave_max_size;
	u32 pkru;
	u8 gdt_copy[GDT_SIZE];
	u8 xsave[PAGE_SIZE];
	u64 restore_gdt_page_va;
	u64 restore_gdt_page_pa;
};

struct sev_snapshot_mailbox {
	struct sev_snapshot_cpu_context cpu_ctx;
	u32 shared_extent_count;
	u32 reserved;
	struct sev_snapshot_shared_extent shared_extents[];
};

#define SEV_SNAPSHOT_CPU_CTX_F_SEGS		BIT(0)
#define SEV_SNAPSHOT_CPU_CTX_F_BASES		BIT(1)
#define SEV_SNAPSHOT_CPU_CTX_F_CONTROL		BIT(2)
#define SEV_SNAPSHOT_CPU_CTX_F_EFER		BIT(3)
#define SEV_SNAPSHOT_CPU_CTX_F_TABLES		BIT(4)
#define SEV_SNAPSHOT_CPU_CTX_F_TR		BIT(5)
#define SEV_SNAPSHOT_CPU_CTX_F_XSAVE_META	BIT(6)
#define SEV_SNAPSHOT_CPU_CTX_F_XSAVE_DATA	BIT(7)
#define SEV_SNAPSHOT_CPU_CTX_F_PKRU		BIT(8)

#define SEV_SNAPSHOT_CPU_CTX_REQUIRED_MASK	\
	(SEV_SNAPSHOT_CPU_CTX_F_SEGS |		\
	 SEV_SNAPSHOT_CPU_CTX_F_BASES |		\
	 SEV_SNAPSHOT_CPU_CTX_F_CONTROL |	\
	 SEV_SNAPSHOT_CPU_CTX_F_EFER |		\
	 SEV_SNAPSHOT_CPU_CTX_F_TABLES |	\
	 SEV_SNAPSHOT_CPU_CTX_F_TR |		\
	 SEV_SNAPSHOT_CPU_CTX_F_PKRU)

static_assert(sizeof(struct sev_snapshot_cpu_context) == 0x11e8);

extern void sev_snapshot_restore_entry_early(struct sev_snapshot_mailbox *mailbox);
extern void sev_snapshot_restore_from_mailbox(struct sev_snapshot_mailbox *mailbox,
					      u64 *bridge_l2e_record);
extern void sev_snapshot_resume_from_ptregs(struct pt_regs *regs);

void sev_snapshot_seed_known_shared_extents(void);
void sev_snapshot_register_boot_ghcb(void);
void sev_snapshot_register_runtime_ghcb(void);
u64 sev_snapshot_runtime_ghcb_pa(void);
void sev_snapshot_track_page_state_pa(unsigned long paddr, unsigned int npages,
				      enum psc_op op);
void sev_snapshot_track_page_state(unsigned long addr, unsigned int npages,
				   enum psc_op op);
void sev_snapshot_track_psc_entries(struct psc_entry *entries,
				    unsigned int nr_entries, enum psc_op op);

int sev_es_ghcb_vmmcall_rax(u64 rax);
int sev_es_ghcb_msr_signal(u64 val);

#endif /* __ARCH_X86_KERNEL_SEV_SNAPSHOT_H */
