// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD SEV-SNP guest-triggered snapshot support.
 *
 * VMPL/VMSA-based guest-triggered snapshot entry path.  The syscall is a
 * checkpoint trigger only; CPU state is captured from the VMPL0-owned VMPL1
 * VMSA page rather than from a Linux-visible mailbox.
 */

#define pr_fmt(fmt) "SEV: " fmt

#include <linux/bug.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/linkage.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/preempt.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/swiotlb.h>
#include <linux/timekeeping.h>
#include <linux/tick.h>
#include <linux/uaccess.h>
#include <linux/virtio.h>
#include <linux/wait.h>

#include <asm/apic.h>
#include <asm/current.h>
#include <asm/cpu_entry_area.h>
#include <asm/desc.h>
#include <asm/fixmap.h>
#include <asm/fpu/api.h>
#include <asm/fpu/sched.h>
#include <asm/fpu/signal.h>
#include <asm/fpu/types.h>
#include <asm/fpu/xcr.h>
#include <asm/gsseg.h>
#include <asm/kvm_para.h>
#include <asm/mem_encrypt.h>
#include <asm/mmu.h>
#include <asm/msr.h>
#include <asm/nmi.h>
#include <asm/paravirt.h>
#include <asm/pkru.h>
#include <asm/proto.h>
#include <asm/processor-flags.h>
#include <asm/sev.h>
#include <asm/segment.h>
#include <asm/smap.h>
#include <asm/smp.h>
#include <asm/special_insns.h>
#include <asm/setup.h>
#include <asm/trap_pf.h>
#include <asm/trapnr.h>
#include <asm/unistd.h>
#include <asm/svm.h>

#include "sev_snapshot.h"

#define SEV_SNAPSHOT_REQUEST_HOST_EVENT_SIGNAL	0x0051000000000014ULL
#define SEV_SNAPSHOT_SHARED_STATE_NOTIFY_VALUE	0xf500000000000014ULL
#define SEV_SNAPSHOT_SHARED_STATE_NOTIFY_MAX_RUN_PAGES	256UL
#define SEV_SNAPSHOT_PAGE_PREPARE_PRIVATE	3

static cpumask_t sev_snapshot_saved_cpus_allowed;
static bool sev_snapshot_saved_cpus_allowed_valid;
static bool sev_snapshot_virtio_devices_frozen;

static int sev_snapshot_pin_current_to_boot_cpu(void)
{
	int ret;

	if (!cpu_online(0))
		return -ENODEV;

	cpumask_copy(&sev_snapshot_saved_cpus_allowed, current->cpus_ptr);
	WRITE_ONCE(sev_snapshot_saved_cpus_allowed_valid, true);

	ret = set_cpus_allowed_ptr(current, cpumask_of(0));
	if (ret) {
		WRITE_ONCE(sev_snapshot_saved_cpus_allowed_valid, false);
		return ret;
	}

	if (raw_smp_processor_id() != 0)
		return -EBUSY;

	return 0;
}

static void sev_snapshot_faasev_current_affinity(void)
{
	cpumask_t allowed;

	if (!READ_ONCE(sev_snapshot_saved_cpus_allowed_valid))
		return;

	cpumask_and(&allowed, &sev_snapshot_saved_cpus_allowed,
		    cpu_online_mask);
	if (cpumask_empty(&allowed))
		cpumask_copy(&allowed, cpu_online_mask);

	if (set_cpus_allowed_ptr(current, &allowed))
		pr_warn("failed to restore snapshot task CPU affinity\n");

	WRITE_ONCE(sev_snapshot_saved_cpus_allowed_valid, false);
}

static void sev_snapshot_abort_capture_setup(void)
{
	if (READ_ONCE(sev_snapshot_virtio_devices_frozen)) {
		if (virtio_sev_snapshot_faasev_devices())
			pr_warn("failed to restore virtio devices after snapshot abort\n");
		else
			WRITE_ONCE(sev_snapshot_virtio_devices_frozen, false);
	}
	sev_snapshot_faasev_current_affinity();
}

int sev_snapshot_freeze_virtio_devices(void)
{
	int ret;

	ret = virtio_sev_snapshot_freeze_devices();
	if (ret) {
		virtio_sev_snapshot_faasev_devices();
		return ret;
	}

	WRITE_ONCE(sev_snapshot_virtio_devices_frozen, true);
	return 0;
}

int sev_snapshot_faasev_virtio_devices(void)
{
	int ret;

	if (!READ_ONCE(sev_snapshot_virtio_devices_frozen))
		return 0;

	ret = virtio_sev_snapshot_faasev_devices();
	if (!ret)
		WRITE_ONCE(sev_snapshot_virtio_devices_frozen, false);
	return ret;
}

static void sev_snapshot_notify_vmpl0_page_state_pa(u64 gpa, u64 npages,
						    u64 op)
{
	unsigned long flags;
	u64 ghcb_msr;

	if (!faasev_vmpl_boot_early || !npages)
		return;
	if (op != SNP_PAGE_STATE_SHARED && op != SNP_PAGE_STATE_PRIVATE &&
	    op != SEV_SNAPSHOT_PAGE_PREPARE_PRIVATE)
		return;

	local_irq_save(flags);
	ghcb_msr = __rdmsr(MSR_AMD64_SEV_ES_GHCB);
	while (npages) {
		u64 chunk = min_t(u64, npages,
				  SEV_SNAPSHOT_SHARED_STATE_NOTIFY_MAX_RUN_PAGES);
		u64 gfn = gpa >> PAGE_SHIFT;
		u64 notify;

		if (WARN_ON_ONCE(gfn > U32_MAX))
			break;

		if (op == SNP_PAGE_STATE_SHARED && npages >= PMD_SIZE / PAGE_SIZE &&
		    swiotlb_is_hugepage_range(gpa, PMD_SIZE)) {
			/* Both VMPL0 firmwares accept a full 2 MiB raw PSC. */
			chunk = PMD_SIZE / PAGE_SIZE;
			notify = GHCB_MSR_PSC_REQ_GFN(gfn, op) | BIT_ULL(63);
		} else {
			notify = SEV_SNAPSHOT_SHARED_STATE_NOTIFY_VALUE |
				 ((u64)op << 52) |
				 ((chunk - 1) << 44) |
				 (gfn << PAGE_SHIFT);
		}
		native_wrmsrl(MSR_AMD64_SEV_ES_GHCB, notify);
		VMGEXIT();

		gpa += chunk << PAGE_SHIFT;
		npages -= chunk;
	}
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB, ghcb_msr);
	local_irq_restore(flags);
}

void sev_snapshot_prepare_private_pages_pa(unsigned long paddr,
					   unsigned int npages)
{
	sev_snapshot_notify_vmpl0_page_state_pa(paddr, npages,
						SEV_SNAPSHOT_PAGE_PREPARE_PRIVATE);
}

void sev_snapshot_notify_page_state_pa(unsigned long paddr, unsigned int npages,
				       enum psc_op op)
{
	if (!npages)
		return;

	if (!cc_platform_has(CC_ATTR_GUEST_SEV_SNP))
		return;

	sev_snapshot_notify_vmpl0_page_state_pa(paddr & PAGE_MASK, npages, op);
}

static void sev_snapshot_seed_initial_shared_range(void)
{
	u64 start, end;

	if (!cc_platform_has(CC_ATTR_GUEST_SEV_SNP))
		return;

	/* VMPL startup clears this mapping's C-bit before Recorder tracking runs. */
	start = ALIGN_DOWN((u64)__pa(__start_bss_decrypted), PAGE_SIZE);
	end = ALIGN((u64)__pa(__start_bss_decrypted_unused), PAGE_SIZE);
	if (start < end)
		sev_snapshot_notify_page_state_pa(start,
						  (end - start) >> PAGE_SHIFT,
						  SNP_PAGE_STATE_SHARED);
}

void sev_snapshot_notify_page_state(unsigned long addr, unsigned int npages,
				    enum psc_op op)
{
	if (!npages)
		return;

	sev_snapshot_notify_page_state_pa(__pa(addr), npages, op);
}

static void sev_snapshot_signal_host_capture_canonical_irqoff(void)
{
	u64 ghcb_msr;

	ghcb_msr = __rdmsr(MSR_AMD64_SEV_ES_GHCB);
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB,
		      SEV_SNAPSHOT_REQUEST_HOST_EVENT_SIGNAL);
	VMGEXIT();
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB, ghcb_msr);
}

static void sev_snapshot_prepare_capture(void)
{
	sev_snapshot_seed_initial_shared_range();
}

static bool sev_snapshot_single_vcpu_online(void)
{
	if (num_online_cpus() != 1)
		return false;

	return cpu_online(0);
}

static int sev_snapshot_refresh_wall_clock(void)
{
	struct timespec64 current_time;
	struct timespec64 host_time;

	/* Match the helper continuation before time-sensitive network requests. */
	read_persistent_clock64(&host_time);
	ktime_get_real_ts64(&current_time);
	if (timespec64_compare(&host_time, &current_time) > 0)
		return do_settimeofday64(&host_time);
	return 0;
}

static long sev_snapshot_capture_vmpl_vmsa_primary(void)
{
	long ret;

	ret = sev_snapshot_freeze_virtio_devices();
	if (ret) {
		sev_snapshot_abort_capture_setup();
		return ret;
	}
	preempt_disable();
	local_irq_disable();

	tick_suspend_local();
	wmb();
	sev_snapshot_signal_host_capture_canonical_irqoff();

	/*
	 * In the VMSA-primary VMPL path, FaaSEV resumes at the instruction
	 * following the checkpoint VMGEXIT.  Run only the fixed Linux-side
	 * post-FaaSEV fixups here; do not deserialize pt_regs or other Linux
	 * continuation state from a checkpoint descriptor.
	 */
	sev_snapshot_register_runtime_ghcb_cpu(0);
	kvm_sev_faasev_reinit();
	/* The new Template LAPIC is not part of the restored VMSA. */
	apic_ap_setup();
	tick_resume_local();
	local_irq_enable();
	preempt_enable();
	ret = sev_snapshot_refresh_wall_clock();
	if (ret) {
		sev_snapshot_abort_capture_setup();
		return ret;
	}
	ret = sev_snapshot_install_input_private_key();
	if (ret) {
		sev_snapshot_abort_capture_setup();
		return ret;
	}
	ret = sev_snapshot_faasev_virtio_devices();
	if (ret) {
		sev_snapshot_faasev_current_affinity();
		return ret;
	}
	sev_snapshot_faasev_current_affinity();
	return 0;
}

long sev_snapshot_arm(void)
{
	long ret;

	if (!faasev_vmpl_boot_early)
		return -EOPNOTSUPP;

	ret = sev_snapshot_prepare_input_crypto();
	if (ret)
		return ret;

	ret = sev_snapshot_pin_current_to_boot_cpu();
	if (ret) {
		sev_snapshot_abort_capture_setup();
		return ret;
	}

	if (!sev_snapshot_single_vcpu_online()) {
		sev_snapshot_abort_capture_setup();
		return -EINVAL;
	}

	sev_snapshot_prepare_capture();

	return sev_snapshot_capture_vmpl_vmsa_primary();
}
