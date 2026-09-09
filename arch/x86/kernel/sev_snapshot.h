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

#include <asm/sev-common.h>

void sev_snapshot_register_runtime_ghcb_cpu(unsigned int cpu);
int sev_snapshot_freeze_virtio_devices(void);
int sev_snapshot_faasev_virtio_devices(void);
void sev_snapshot_notify_page_state_pa(unsigned long paddr, unsigned int npages,
				       enum psc_op op);
void sev_snapshot_notify_page_state(unsigned long addr, unsigned int npages,
				    enum psc_op op);

#endif /* __ARCH_X86_KERNEL_SEV_SNAPSHOT_H */
