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
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/linkage.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/preempt.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tick.h>
#include <linux/uaccess.h>
#include <linux/virtio.h>
#include <linux/wait.h>

#include <asm/current.h>
#include <asm/apic.h>
#include <asm/apicdef.h>
#include <asm/cacheflush.h>
#include <asm/cpu_entry_area.h>
#include <asm/desc.h>
#include <asm/e820/api.h>
#include <asm/fixmap.h>
#include <asm/fpu/api.h>
#include <asm/fpu/sched.h>
#include <asm/fpu/signal.h>
#include <asm/fpu/types.h>
#include <asm/fpu/xcr.h>
#include <asm/gsseg.h>
#include <asm/irq_regs.h>
#include <asm/irq_vectors.h>
#include <asm/kvm_para.h>
#include <asm/mem_encrypt.h>
#include <asm/mmu.h>
#include <asm/msr.h>
#include <asm/nmi.h>
#include <asm/paravirt.h>
#include <asm/pgtable.h>
#include <asm/pgtable_types.h>
#include <asm/pkru.h>
#include <asm/proto.h>
#include <asm/processor-flags.h>
#include <asm/sev.h>
#include <asm/segment.h>
#include <asm/set_memory.h>
#include <asm/smap.h>
#include <asm/smp.h>
#include <asm/special_insns.h>
#include <asm/setup.h>
#include <asm/trap_pf.h>
#include <asm/trapnr.h>
#include <asm/tlbflush.h>
#include <asm/unistd.h>
#include <asm/svm.h>

#include "sev_snapshot.h"

#define SEV_SNAPSHOT_CONTROL_SHARED_PA		0x001c0000ULL
#define SEV_SNAPSHOT_CONTROL_SHARED_SIZE	(SZ_256K)
#define SEV_SNAPSHOT_ATTESTATION_REQUEST_OFFSET \
	(SEV_SNAPSHOT_CONTROL_SHARED_SIZE - (2 * PAGE_SIZE))
#define SEV_SNAPSHOT_REQUEST_HOST_EVENT_SIGNAL	0x0051000000000014ULL
#define SEV_SNAPSHOT_SHARED_STATE_NOTIFY_VALUE	0xf500000000000014ULL
#define SEV_SNAPSHOT_SHARED_STATE_NOTIFY_MAX_RUN_PAGES	256UL
#define SEV_SNAPSHOT_APIC_DIVISOR		16
#define SEV_SNAPSHOT_FALLBACK_AP_TMICT		0x3d091U
#define SEV_SNAPSHOT_SHARED_EXTENT_MAX		32768

static raw_spinlock_t sev_snapshot_shared_lock;
static DEFINE_MUTEX(sev_snapshot_transport_lock);
static bool sev_snapshot_shared_ready;
static void *sev_snapshot_attestation_request_page_va;
static bool sev_snapshot_warned_extent_capacity;
static cpumask_t sev_snapshot_saved_cpus_allowed;
static bool sev_snapshot_saved_cpus_allowed_valid;
static bool sev_snapshot_vmpl_mode;
static bool sev_snapshot_virtio_devices_frozen;
static struct sev_snapshot_shared_extent
	sev_snapshot_shared_extents[SEV_SNAPSHOT_SHARED_EXTENT_MAX];
static u32 sev_snapshot_shared_extent_count;

extern char __init_scratch_begin[], __init_scratch_end[];
extern void sev_snapshot_lapic_timer_restore_periodic_handler(void);

static void sev_snapshot_map_request_page_locked(void);
static void sev_snapshot_resume_local_tick(void);
static void sev_snapshot_rearm_boot_local_apic_timer(void);
static void sev_snapshot_quiesce_local_apic_timer(void);

static __always_inline void sev_snapshot_restore_debug_code(u8 code)
{
	unsigned long flags;
	u64 ghcb_msr;

	local_irq_save(flags);
	ghcb_msr = __rdmsr(MSR_AMD64_SEV_ES_GHCB);
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB, ((u64)code << 48) | 0x14);
	VMGEXIT();
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB, ghcb_msr);
	local_irq_restore(flags);
}

static int __init sev_snapshot_vmpl_mode_setup(char *arg)
{
	if (!arg || !strcmp(arg, "1") || !strcmp(arg, "y") ||
	    !strcmp(arg, "Y") || !strcmp(arg, "true"))
		sev_snapshot_vmpl_mode = true;
	else
		sev_snapshot_vmpl_mode = false;

	return 0;
}
early_param("faasev_vmpl", sev_snapshot_vmpl_mode_setup);

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

static void sev_snapshot_restore_current_affinity(void)
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

static void sev_snapshot_clear_idle_polling_flags(void)
{
	struct task_struct *idle;
	bool cleared = false;
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		idle = idle_task(cpu);
		if (!idle)
			continue;
		if (test_and_clear_tsk_thread_flag(idle, TIF_POLLING_NRFLAG))
			cleared = true;
	}

	if (!cleared)
		return;

	smp_mb__after_atomic();
}

static void sev_snapshot_drain_local_apic_isr(void)
{
	unsigned long flags;
	unsigned int iter;
	unsigned int reg;

	local_irq_save(flags);
	for (iter = 0; iter < APIC_ISR_NR * 32; iter++) {
		bool has_isr = false;

		for (reg = 0; reg < APIC_ISR_NR; reg++) {
			if (apic_read(APIC_ISR + reg * 0x10)) {
				has_isr = true;
				break;
			}
		}
		if (!has_isr)
			break;

		ack_APIC_irq();
	}
	local_irq_restore(flags);
}

static void sev_snapshot_abort_capture_setup(void)
{
	if (READ_ONCE(sev_snapshot_virtio_devices_frozen)) {
		if (virtio_sev_snapshot_restore_devices())
			pr_warn("failed to restore virtio devices after snapshot abort\n");
		WRITE_ONCE(sev_snapshot_virtio_devices_frozen, false);
	}
	sev_snapshot_restore_current_affinity();
}

static int sev_snapshot_freeze_virtio_devices(void)
{
	int ret;

	ret = virtio_sev_snapshot_freeze_devices();
	if (ret) {
		virtio_sev_snapshot_restore_devices();
		return ret;
	}

	WRITE_ONCE(sev_snapshot_virtio_devices_frozen, true);
	return 0;
}

static int sev_snapshot_restore_virtio_devices(void)
{
	int ret;

	if (!READ_ONCE(sev_snapshot_virtio_devices_frozen))
		return 0;

	local_irq_enable();
	ret = virtio_sev_snapshot_restore_devices();
	local_irq_disable();
	if (ret)
		return ret;

	WRITE_ONCE(sev_snapshot_virtio_devices_frozen, false);
	return 0;
}

static phys_addr_t sev_snapshot_transport_page_pa(u64 offset)
{
	if (offset > SEV_SNAPSHOT_CONTROL_SHARED_SIZE ||
	    PAGE_SIZE > SEV_SNAPSHOT_CONTROL_SHARED_SIZE - offset)
		return 0;

	return SEV_SNAPSHOT_CONTROL_SHARED_PA + offset;
}

static int sev_snapshot_map_transport_pages(void)
{
	mutex_lock(&sev_snapshot_transport_lock);
	sev_snapshot_map_request_page_locked();
	mutex_unlock(&sev_snapshot_transport_lock);

	if (!READ_ONCE(sev_snapshot_attestation_request_page_va))
		return -ENOMEM;

	return 0;
}

static void sev_snapshot_remove_extent_locked(struct sev_snapshot_shared_extent *extents,
					      u32 *count, u32 max_count,
					      u64 gpa, u64 npages)
{
	u64 end = gpa + (npages << PAGE_SHIFT);
	u32 i;

	for (i = 0; i < *count;) {
		struct sev_snapshot_shared_extent *extent = &extents[i];
		u64 extent_start = extent->gpa;
		u64 extent_end = extent->gpa + (extent->npages << PAGE_SHIFT);

		if (end <= extent_start || gpa >= extent_end) {
			i++;
			continue;
		}

		if (gpa <= extent_start && end >= extent_end) {
			memmove(extent, extent + 1,
				(*count - i - 1) *
				sizeof(*extent));
			(*count)--;
			continue;
		}

		if (gpa <= extent_start) {
			extent->gpa = end;
			extent->npages = (extent_end - end) >> PAGE_SHIFT;
			i++;
			continue;
		}

		if (end >= extent_end) {
			extent->npages = (gpa - extent_start) >> PAGE_SHIFT;
			i++;
			continue;
		}

		if (*count >= max_count) {
			extent->npages = (gpa - extent_start) >> PAGE_SHIFT;
			i++;
			continue;
		}

		memmove(extent + 2, extent + 1,
			(*count - i - 1) *
			sizeof(*extent));
		(*count)++;
		extent[1].gpa = end;
		extent[1].npages = (extent_end - end) >> PAGE_SHIFT;
		extent->npages = (gpa - extent_start) >> PAGE_SHIFT;
		i += 2;
	}
}

static bool sev_snapshot_add_extent_locked(struct sev_snapshot_shared_extent *extents,
					   u32 *count, u32 max_count,
					   u64 gpa, u64 npages)
{
	u64 end = gpa + (npages << PAGE_SHIFT);
	u32 i;

	sev_snapshot_remove_extent_locked(extents, count, max_count, gpa, npages);

	for (i = 0; i < *count; i++) {
		struct sev_snapshot_shared_extent *extent = &extents[i];
		u64 extent_start = extent->gpa;
		u64 extent_end = extent->gpa + (extent->npages << PAGE_SHIFT);

		if (extent_end == gpa) {
			extent->npages += npages;
			return true;
		}

		if (end == extent_start) {
			extent->gpa = gpa;
			extent->npages += npages;
			return true;
		}
	}

	if (*count >= max_count)
		return false;

	extents[*count].gpa = gpa;
	extents[*count].npages = npages;
	(*count)++;
	return true;
}

static void sev_snapshot_remove_shared_extent_locked(u64 gpa, u64 npages)
{
	sev_snapshot_remove_extent_locked(sev_snapshot_shared_extents,
					  &sev_snapshot_shared_extent_count,
					  SEV_SNAPSHOT_SHARED_EXTENT_MAX,
					  gpa, npages);
}

static void sev_snapshot_add_shared_extent_locked(u64 gpa, u64 npages)
{
	if (!sev_snapshot_add_extent_locked(sev_snapshot_shared_extents,
					    &sev_snapshot_shared_extent_count,
					    SEV_SNAPSHOT_SHARED_EXTENT_MAX,
					    gpa, npages)) {
		if (!sev_snapshot_warned_extent_capacity) {
			pr_warn("SEV snapshot shared extent table is full\n");
			sev_snapshot_warned_extent_capacity = true;
		}
	}
}

static void sev_snapshot_notify_vmpl0_page_state_pa(u64 gpa, u64 npages,
						    enum psc_op op)
{
	unsigned long flags;
	u64 ghcb_msr;

	if (!sev_snapshot_vmpl_mode || !npages)
		return;
	if (op != SNP_PAGE_STATE_SHARED && op != SNP_PAGE_STATE_PRIVATE)
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

		notify = SEV_SNAPSHOT_SHARED_STATE_NOTIFY_VALUE |
			 ((u64)op << 52) |
			 ((chunk - 1) << 44) |
			 (gfn << PAGE_SHIFT);
		native_wrmsrl(MSR_AMD64_SEV_ES_GHCB, notify);
		VMGEXIT();

		gpa += chunk << PAGE_SHIFT;
		npages -= chunk;
	}
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB, ghcb_msr);
	local_irq_restore(flags);
}

static bool sev_snapshot_shared_extent_contains_locked(u64 gpa, u64 npages)
{
	u64 end = gpa + (npages << PAGE_SHIFT);
	u32 i;

	for (i = 0; i < sev_snapshot_shared_extent_count; i++) {
		struct sev_snapshot_shared_extent *extent =
			&sev_snapshot_shared_extents[i];
		u64 extent_start = extent->gpa;
		u64 extent_end = extent->gpa + (extent->npages << PAGE_SHIFT);

		if (extent_start <= gpa && end <= extent_end)
			return true;
	}

	return false;
}

static bool sev_snapshot_shared_extent_contains(u64 gpa, u64 npages)
{
	unsigned long flags;
	bool contains;

	raw_spin_lock_irqsave(&sev_snapshot_shared_lock, flags);
	contains = sev_snapshot_shared_extent_contains_locked(gpa, npages);
	raw_spin_unlock_irqrestore(&sev_snapshot_shared_lock, flags);

	return contains;
}

void sev_snapshot_track_page_state_pa(unsigned long paddr, unsigned int npages,
				      enum psc_op op)
{
	unsigned long flags;
	u64 gpa;
	bool notify = false;

	if (!npages)
		return;

	if (!cc_platform_has(CC_ATTR_GUEST_SEV_SNP))
		return;

	gpa = paddr & PAGE_MASK;

	raw_spin_lock_irqsave(&sev_snapshot_shared_lock, flags);
	if (op == SNP_PAGE_STATE_SHARED) {
		sev_snapshot_add_shared_extent_locked(gpa, npages);
		notify = true;
	} else if (op == SNP_PAGE_STATE_PRIVATE) {
		sev_snapshot_remove_shared_extent_locked(gpa, npages);
		notify = true;
	}
	raw_spin_unlock_irqrestore(&sev_snapshot_shared_lock, flags);

	if (notify)
		sev_snapshot_notify_vmpl0_page_state_pa(gpa, npages, op);
}

static bool sev_snapshot_direct_map_entry(unsigned long vaddr,
					  unsigned int *level,
					  unsigned long *entry,
					  unsigned long *size)
{
	pte_t *kpte;

	kpte = lookup_address(vaddr, level);
	if (!kpte || pte_none(*kpte))
		return false;

	if (*level != PG_LEVEL_4K && *level != PG_LEVEL_2M &&
	    *level != PG_LEVEL_1G)
		return false;

	*entry = pte_val(*kpte);
	if (!(*entry & _PAGE_PRESENT))
		return false;

	*size = page_level_size(*level);
	return true;
}

static int sev_snapshot_encrypt_init_scratch_run(unsigned long *run_start,
						 unsigned long *run_npages)
{
	int ret;

	if (!*run_npages)
		return 0;

	ret = set_memory_encrypted(*run_start, *run_npages);
	if (ret)
		pr_warn("failed to make init scratch private [mem %#010lx-%#010lx]\n",
			*run_start, *run_start + (*run_npages << PAGE_SHIFT) - 1);

	*run_start = 0;
	*run_npages = 0;
	return ret;
}

static int sev_snapshot_encrypt_init_scratch_decrypted_pages(void)
{
	unsigned long paddr, paddr_end, run_start = 0, run_npages = 0;
	int ret;

	if (!cc_platform_has(CC_ATTR_GUEST_SEV_SNP) || !sme_me_mask)
		return 0;

	paddr = ALIGN_DOWN(__pa_symbol(__init_scratch_begin), PAGE_SIZE);
	paddr_end = PAGE_ALIGN(__pa_symbol(__init_scratch_end));

	for (; paddr < paddr_end; paddr += PAGE_SIZE) {
		unsigned long vaddr = (unsigned long)__va(paddr);
		unsigned long entry, size;
		unsigned int level;
		bool decrypted;

		decrypted = sev_snapshot_direct_map_entry(vaddr, &level, &entry,
							  &size) &&
			    !(entry & sme_me_mask);
		if (!decrypted ||
		    sev_snapshot_shared_extent_contains(paddr, 1)) {
			ret = sev_snapshot_encrypt_init_scratch_run(&run_start,
								   &run_npages);
			if (ret)
				return ret;
			continue;
		}

		if (!run_npages)
			run_start = vaddr;
		run_npages++;
	}

	return sev_snapshot_encrypt_init_scratch_run(&run_start, &run_npages);
}

static void sev_snapshot_rebuild_shared_extents_from_direct_map(void)
{
	unsigned long end_pfn = max_pfn_mapped;
	unsigned long pfn;

	if (!cc_platform_has(CC_ATTR_GUEST_SEV_SNP) || !sme_me_mask)
		return;

	for (pfn = 0; pfn < end_pfn;) {
		unsigned long paddr = PFN_PHYS(pfn);
		unsigned long vaddr = (unsigned long)__va(paddr);
		unsigned long entry, size, npages;
		unsigned int level;

		if (!sev_snapshot_direct_map_entry(vaddr, &level, &entry,
						   &size)) {
			pfn++;
			continue;
		}

		npages = min_t(unsigned long, size >> PAGE_SHIFT,
			       end_pfn - pfn);
		if (!npages) {
			pfn++;
			continue;
		}

		if (!(entry & sme_me_mask)) {
			unsigned long flags;

			raw_spin_lock_irqsave(&sev_snapshot_shared_lock, flags);
			sev_snapshot_add_shared_extent_locked(paddr, npages);
			raw_spin_unlock_irqrestore(&sev_snapshot_shared_lock, flags);
		}

		pfn += npages;
	}
}

static void sev_snapshot_shared_init(void)
{
	bool do_seed = false;
	unsigned long flags;
	u64 start, end;

	raw_spin_lock_irqsave(&sev_snapshot_shared_lock, flags);

	if (sev_snapshot_shared_ready)
		goto out_unlock;

	sev_snapshot_shared_ready = true;
	do_seed = true;

out_unlock:
	raw_spin_unlock_irqrestore(&sev_snapshot_shared_lock, flags);

	if (!do_seed)
		return;

	if (cc_platform_has(CC_ATTR_GUEST_SEV_SNP)) {
		start = ALIGN_DOWN((u64)__pa(__start_bss_decrypted), PAGE_SIZE);
		end = ALIGN((u64)__pa(__start_bss_decrypted_unused), PAGE_SIZE);
		if (start < end)
			sev_snapshot_track_page_state_pa(start,
							 (end - start) >> PAGE_SHIFT,
							 SNP_PAGE_STATE_SHARED);
	}

	sev_snapshot_seed_known_shared_extents();
}

static void sev_snapshot_map_request_page_locked(void)
{
	phys_addr_t request_page_pa;

	if (sev_snapshot_attestation_request_page_va)
		return;

	request_page_pa =
		sev_snapshot_transport_page_pa(SEV_SNAPSHOT_ATTESTATION_REQUEST_OFFSET);
	if (!request_page_pa)
		return;

	sev_snapshot_attestation_request_page_va =
		memremap(request_page_pa, PAGE_SIZE, MEMREMAP_WB | MEMREMAP_DEC);
	if (!sev_snapshot_attestation_request_page_va)
		return;

}

void sev_snapshot_track_page_state(unsigned long addr, unsigned int npages,
				   enum psc_op op)
{
	if (!npages)
		return;

	sev_snapshot_track_page_state_pa(__pa(addr), npages, op);
}

void sev_snapshot_track_psc_entries(struct psc_entry *entries,
				    unsigned int nr_entries, enum psc_op op)
{
	unsigned long range_start = 0;
	unsigned long next_gpa = 0;
	unsigned int i;

	if (!entries || !nr_entries)
		return;

	for (i = 0; i < nr_entries; i++) {
		unsigned long gpa = (unsigned long)entries[i].gfn << PAGE_SHIFT;

		if (!i) {
			range_start = gpa;
			next_gpa = gpa + PAGE_SIZE;
			continue;
		}

		if (gpa == next_gpa) {
			next_gpa += PAGE_SIZE;
			continue;
		}

		sev_snapshot_track_page_state_pa(range_start,
						 (next_gpa - range_start) >> PAGE_SHIFT,
						 op);
		range_start = gpa;
		next_gpa = gpa + PAGE_SIZE;
	}

	sev_snapshot_track_page_state_pa(range_start,
					 (next_gpa - range_start) >> PAGE_SHIFT,
					 op);
}

static void sev_snapshot_suspend_local_tick(void)
{
	unsigned long flags;

	local_irq_save(flags);
	tick_suspend_local();
	local_irq_restore(flags);
}

static void sev_snapshot_resume_local_tick(void)
{
	unsigned long flags;

	local_irq_save(flags);
	tick_resume_local();
	local_irq_restore(flags);
}

static void sev_snapshot_rearm_boot_local_apic_timer(void)
{
	unsigned int tmict = SEV_SNAPSHOT_FALLBACK_AP_TMICT;
	unsigned long flags;

	if (lapic_timer_period)
		tmict = lapic_timer_period / SEV_SNAPSHOT_APIC_DIVISOR;
	if (!tmict)
		tmict = SEV_SNAPSHOT_FALLBACK_AP_TMICT;

	local_irq_save(flags);
	apic_write(APIC_TASKPRI, 0);
	apic_write(APIC_SPIV, APIC_SPIV_APIC_ENABLED | SPURIOUS_APIC_VECTOR);
	apic_write(APIC_TDCR, APIC_TDR_DIV_16);
	apic_write(APIC_LVTT, APIC_LVT_TIMER_PERIODIC | LOCAL_TIMER_VECTOR);
	apic_write(APIC_TMICT, tmict);
	local_irq_restore(flags);
}

static int sev_snapshot_signal_host_capture_canonical_irqoff(void)
{
	u64 ghcb_msr;

	local_irq_disable();
	ghcb_msr = __rdmsr(MSR_AMD64_SEV_ES_GHCB);
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB,
		      SEV_SNAPSHOT_REQUEST_HOST_EVENT_SIGNAL);
	VMGEXIT();
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB, ghcb_msr);

	return 0;
}

static int sev_snapshot_prepare_capture(void)
{
	long ret;

	ret = sev_snapshot_map_transport_pages();
	if (ret)
		return ret;

	sev_snapshot_shared_init();
	ret = sev_snapshot_encrypt_init_scratch_decrypted_pages();
	if (ret)
		return ret;

	return 0;
}

static bool sev_snapshot_single_vcpu_online(void)
{
	if (num_online_cpus() != SEV_SNAPSHOT_V2_MIN_CPUS)
		return false;

	return cpu_online(0);
}

static long sev_snapshot_capture_vmpl_vmsa_primary(void)
{
	long ret;

	if (raw_smp_processor_id())
		return -EINVAL;

	if (!sev_snapshot_single_vcpu_online())
		return -EINVAL;

	sev_snapshot_restore_debug_code(0x73);
	ret = sev_snapshot_freeze_virtio_devices();
	if (ret)
		return ret;
	sev_snapshot_restore_debug_code(0x75);

	preempt_disable();
	local_irq_disable();

	sev_snapshot_suspend_local_tick();
	sev_snapshot_quiesce_local_apic_timer();
	sev_snapshot_restore_debug_code(0x77);
	sev_snapshot_rebuild_shared_extents_from_direct_map();
	sev_snapshot_restore_debug_code(0x78);
	wmb();
	sev_snapshot_restore_debug_code(0x79);

	ret = sev_snapshot_signal_host_capture_canonical_irqoff();
	if (ret)
		goto out_restore_irqoff;
	sev_snapshot_restore_debug_code(0x7a);

	/*
	 * In the VMSA-primary VMPL path, restore resumes at the instruction
	 * following the checkpoint VMGEXIT.  Run only the fixed Linux-side
	 * post-restore fixups here; do not deserialize pt_regs or other Linux
	 * continuation state from a checkpoint descriptor.
	 */
	sev_snapshot_register_runtime_ghcb_cpu(0);
	sev_snapshot_restore_debug_code(0x7b);
	if (!sev_snapshot_negotiate_runtime_ghcb_protocol()) {
		ret = -EIO;
		goto out_restore_irqoff;
	}
	sev_snapshot_restore_debug_code(0xaa);
	kvm_sev_snapshot_restore_reinit();
	sev_snapshot_restore_debug_code(0xab);
	sev_snapshot_drain_local_apic_isr();
	sev_snapshot_clear_idle_polling_flags();
	tick_broadcast_control(TICK_BROADCAST_OFF);
	sev_snapshot_restore_debug_code(0xb0);
	ret = sev_snapshot_restore_virtio_devices();
	if (ret)
		goto out_restore_irqoff;
	sev_snapshot_resume_local_tick();
	sev_snapshot_lapic_timer_restore_periodic_handler();
	sev_snapshot_rearm_boot_local_apic_timer();
	sev_snapshot_restore_debug_code(0xef);

	local_irq_enable();
	preempt_enable();
	sev_snapshot_restore_current_affinity();
	return 0;

out_restore_irqoff:
	local_irq_enable();
	preempt_enable();
	sev_snapshot_abort_capture_setup();
	return ret;
}

long sev_snapshot_arm(void)
{
	long ret;

	if (!sev_snapshot_vmpl_mode)
		return -EOPNOTSUPP;

	ret = sev_snapshot_pin_current_to_boot_cpu();
	if (ret) {
		sev_snapshot_abort_capture_setup();
		return ret;
	}

	if (!sev_snapshot_single_vcpu_online()) {
		sev_snapshot_abort_capture_setup();
		return -EINVAL;
	}

	ret = sev_snapshot_prepare_capture();
	if (ret) {
		sev_snapshot_abort_capture_setup();
		return ret;
	}

	return sev_snapshot_capture_vmpl_vmsa_primary();
}

static void sev_snapshot_quiesce_local_apic_timer(void)
{
	unsigned long flags;

	local_irq_save(flags);
	apic_write(APIC_LVTT, APIC_LVT_MASKED | LOCAL_TIMER_VECTOR);
	apic_write(APIC_TMICT, 0);
	local_irq_restore(flags);
}
