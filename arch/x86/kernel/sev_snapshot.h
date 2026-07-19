/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AMD SEV-SNP guest-triggered snapshot support.
 *
 * This header is intentionally local to arch/x86/kernel/.  The snapshot
 * syscall is now a VMPL/VMSA handoff trigger; Linux no longer publishes a
 * restore descriptor or CPU-state mailbox.
 */

#ifndef __ARCH_X86_KERNEL_SEV_SNAPSHOT_H
#define __ARCH_X86_KERNEL_SEV_SNAPSHOT_H

#include <linux/types.h>

#include <asm/bootparam.h>
#include <asm/sev-common.h>

struct sev_snapshot_shared_extent {
	u64 gpa;
	u64 npages;
};

#define SEV_SNAPSHOT_V2_MIN_CPUS		1U

void sev_snapshot_seed_known_shared_extents(void);
void sev_snapshot_reset_offline_ghcb_state(void);
void sev_snapshot_register_boot_ghcb(void);
void sev_snapshot_register_runtime_ghcb(void);
void sev_snapshot_register_runtime_ghcb_cpu(unsigned int cpu);
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
