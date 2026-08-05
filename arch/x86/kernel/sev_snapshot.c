// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD SEV-SNP guest-triggered snapshot support.
 *
 * This file is reconstructed from the previously built object and the current
 * in-tree contracts. The exact comments/formatting of the lost source are not
 * recoverable, but the mailbox ABI, boot-time reservation path, runtime
 * snapshot entry path, and restore glue are restored here.
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

#define SEV_SNAPSHOT_MAILBOX_SIZE		SZ_2M
#define SEV_SNAPSHOT_FIXED_MAILBOX_PA		0x00e00000ULL
#define SEV_SNAPSHOT_CONTROL_SHARED_PA		0x00200000ULL
#define SEV_SNAPSHOT_CONTROL_SHARED_SIZE	SZ_2M
#define SEV_SNAPSHOT_ATTESTATION_REQUEST_OFFSET \
	(SEV_SNAPSHOT_CONTROL_SHARED_SIZE - (2 * PAGE_SIZE))
#define SEV_SNAPSHOT_REQUEST_HOST_EVENT_SIGNAL	0x0051000000000014ULL
#define SEV_SNAPSHOT_APIC_DIVISOR		16
#define SEV_SNAPSHOT_FALLBACK_AP_TMICT		0x3d091U

#define SEV_SNAPSHOT_SHARED_EXTENT_MAX		32768

static u8 sev_snapshot_restore_xsave[PAGE_SIZE] __aligned(64);
static u8 sev_snapshot_restore_gdt_pages[SEV_SNAPSHOT_V2_MAX_CPUS][PAGE_SIZE]
	__page_aligned_bss;

static phys_addr_t sev_snapshot_mailbox_pa;
static u64 sev_snapshot_mailbox_len;
static raw_spinlock_t sev_snapshot_shared_lock;
static DEFINE_MUTEX(sev_snapshot_transport_lock);
static bool sev_snapshot_shared_ready;
static DEFINE_PER_CPU(struct task_struct *, sev_snapshot_pending_task);
static void *sev_snapshot_attestation_request_page_va;
static bool sev_snapshot_warned_extent_capacity;
bool sev_snapshot_resume_in_progress;
static cpumask_t sev_snapshot_saved_cpus_allowed;
static bool sev_snapshot_saved_cpus_allowed_valid;
static unsigned int sev_snapshot_restore_mode = SEV_SNAPSHOT_RESTORE_MODE_SINGLE_VCPU;
static atomic_t sev_snapshot_v2_ap_ready = ATOMIC_INIT(0);
static atomic_t sev_snapshot_v2_ap_error = ATOMIC_INIT(0);
static atomic_t sev_snapshot_v2_ap_request_epoch = ATOMIC_INIT(0);
static u32 sev_snapshot_v2_ap_cpu_count;
static struct sev_snapshot_mailbox_v2 *sev_snapshot_v2_ap_mailbox;
static struct task_struct *sev_snapshot_v2_ap_tasks[SEV_SNAPSHOT_V2_MAX_CPUS];
static bool sev_snapshot_v2_ap_restore_escape;
static DEFINE_MUTEX(sev_snapshot_v2_ap_task_lock);
static DECLARE_WAIT_QUEUE_HEAD(sev_snapshot_v2_ap_waitq);
static struct pt_regs sev_snapshot_v2_ap_regs[SEV_SNAPSHOT_V2_MAX_CPUS];
/* Rollback note: disable these hooks together with Firecracker host replay re-enablement. */
static bool sev_snapshot_virtio_devices_frozen;
unsigned long sev_snapshot_restore_user_return_sp0;
static struct sev_snapshot_shared_extent
	sev_snapshot_shared_extents[SEV_SNAPSHOT_SHARED_EXTENT_MAX];
static u32 sev_snapshot_shared_extent_count;

extern char __init_scratch_begin[], __init_scratch_end[];

static void sev_snapshot_map_request_page_locked(void);
static void sev_snapshot_resume_local_tick(void);
static void sev_snapshot_rearm_boot_local_apic_timer(void);
static bool sev_snapshot_mailbox_is_v2(struct sev_snapshot_mailbox *mailbox);
static void sev_snapshot_restore_v2_ap_continue_after_ptregs(void);
static void sev_snapshot_restore_v2_ap_wait_after_context_restore(void);
static __always_inline void __noreturn sev_snapshot_restore_fault(void);
static u32 *
sev_snapshot_mailbox_v2_shared_extent_count(struct sev_snapshot_mailbox_v2 *mailbox);
static struct sev_snapshot_shared_extent *
sev_snapshot_mailbox_v2_shared_extents(struct sev_snapshot_mailbox_v2 *mailbox);

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

static __always_inline void sev_snapshot_restore_cpu_debug_code(u8 base,
								u64 cpu)
{
	if (cpu < 2)
		sev_snapshot_restore_debug_code(base + (u8)cpu);
}

static int __init sev_snapshot_restore_mode_setup(char *arg)
{
	if (!arg)
		return 0;
	if (!strcmp(arg, "multi_vcpu"))
		sev_snapshot_restore_mode =
			SEV_SNAPSHOT_RESTORE_MODE_MULTI_VCPU;
	else if (!strcmp(arg, "single_vcpu"))
		sev_snapshot_restore_mode =
			SEV_SNAPSHOT_RESTORE_MODE_SINGLE_VCPU;
	else
		pr_warn("unknown sev_snapshot_restore_mode=%s, using single_vcpu\n",
			arg);
	return 0;
}
early_param("sev_snapshot_restore_mode", sev_snapshot_restore_mode_setup);

static __always_inline struct sev_snapshot_mailbox *sev_snapshot_mailbox_va(void)
{
	if (!sev_snapshot_mailbox_pa || !sev_snapshot_mailbox_len)
		return NULL;

	return __va(sev_snapshot_mailbox_pa);
}

static __always_inline struct sev_snapshot_mailbox *sev_snapshot_restore_mailbox_va(void)
{
	return __va(SEV_SNAPSHOT_FIXED_MAILBOX_PA);
}

static __always_inline u8 *sev_snapshot_restore_gdt_page_for_cpu(unsigned int cpu)
{
	if (cpu >= SEV_SNAPSHOT_V2_MAX_CPUS)
		sev_snapshot_restore_fault();
	return sev_snapshot_restore_gdt_pages[cpu];
}

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

static void sev_snapshot_install_sp0_scratch_stack(void)
{
#ifdef CONFIG_X86_64
	unsigned long scratch_top;

	scratch_top = (unsigned long)(cpu_entry_stack(raw_smp_processor_id()) + 1);
	load_sp0(scratch_top);
	if (!raw_smp_processor_id())
		WRITE_ONCE(sev_snapshot_restore_user_return_sp0, scratch_top);
#endif
}

static void sev_snapshot_restore_irq_stack_ptr(
	const struct sev_snapshot_cpu_context *ctx, unsigned int cpu)
{
#ifdef CONFIG_X86_64
	if (!ctx || !ctx->hardirq_stack_ptr)
		return;

	per_cpu(pcpu_hot.hardirq_stack_ptr, cpu) =
		(void *)(unsigned long)ctx->hardirq_stack_ptr;
#endif
}

static void sev_snapshot_enter_irq_stack_restore_window(
	const struct sev_snapshot_cpu_context *ctx, unsigned int cpu)
{
#ifdef CONFIG_X86_64
	sev_snapshot_restore_irq_stack_ptr(ctx, cpu);
	per_cpu(pcpu_hot.hardirq_stack_inuse, cpu) = true;
#endif
}

static void sev_snapshot_leave_irq_stack_restore_window(
	const struct sev_snapshot_cpu_context *ctx, unsigned int cpu)
{
#ifdef CONFIG_X86_64
	sev_snapshot_restore_irq_stack_ptr(ctx, cpu);
	per_cpu(pcpu_hot.hardirq_stack_inuse, cpu) = false;
#endif
}

static bool __init sev_snapshot_range_is_ram(u64 start, u64 size)
{
	u64 end;

	if (!size || check_add_overflow(start, size, &end))
		return false;

	return e820__mapped_all(start, end, E820_TYPE_RAM);
}

static phys_addr_t sev_snapshot_transport_page_pa(u64 offset)
{
	if (!sev_snapshot_mailbox_pa || !sev_snapshot_mailbox_len)
		return 0;

	if (offset > SEV_SNAPSHOT_CONTROL_SHARED_SIZE ||
	    PAGE_SIZE > SEV_SNAPSHOT_CONTROL_SHARED_SIZE - offset)
		return 0;

	return SEV_SNAPSHOT_CONTROL_SHARED_PA + offset;
}

static int sev_snapshot_map_transport_pages(void)
{
	if (!sev_snapshot_mailbox_pa || !sev_snapshot_mailbox_len)
		return -ENODEV;

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

	if (!npages)
		return;

	if (!cc_platform_has(CC_ATTR_GUEST_SEV_SNP))
		return;

	gpa = paddr & PAGE_MASK;

	raw_spin_lock_irqsave(&sev_snapshot_shared_lock, flags);
	if (op == SNP_PAGE_STATE_SHARED)
		sev_snapshot_add_shared_extent_locked(gpa, npages);
	else if (op == SNP_PAGE_STATE_PRIVATE)
		sev_snapshot_remove_shared_extent_locked(gpa, npages);
	raw_spin_unlock_irqrestore(&sev_snapshot_shared_lock, flags);
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

		if (!(entry & sme_me_mask))
			sev_snapshot_track_page_state_pa(paddr, npages,
							 SNP_PAGE_STATE_SHARED);

		pfn += npages;
	}
}

static void sev_snapshot_flush_shared_extent_array(
	u32 *shared_extent_count,
	struct sev_snapshot_shared_extent *shared_extents,
	size_t header_len)
{
	unsigned long flags;
	u32 capacity, count;

	if (!shared_extent_count || !shared_extents || header_len >= sev_snapshot_mailbox_len)
		return;

	raw_spin_lock_irqsave(&sev_snapshot_shared_lock, flags);
	capacity = (sev_snapshot_mailbox_len - header_len) / sizeof(*shared_extents);
	count = min_t(u32, sev_snapshot_shared_extent_count,
		      SEV_SNAPSHOT_SHARED_EXTENT_MAX);
	count = min(count, capacity);
	memcpy(shared_extents, sev_snapshot_shared_extents,
	       count * sizeof(*shared_extents));
	WRITE_ONCE(*shared_extent_count, count);
	raw_spin_unlock_irqrestore(&sev_snapshot_shared_lock, flags);
}

static void sev_snapshot_flush_shared_extents_v2(
	struct sev_snapshot_mailbox_v2 *mailbox)
{
	size_t shared_extents_offset;

	if (!mailbox)
		return;

	shared_extents_offset = READ_ONCE(mailbox->header.cpu_bundle_len) +
				2 * sizeof(u32);
	sev_snapshot_flush_shared_extent_array(
		sev_snapshot_mailbox_v2_shared_extent_count(mailbox),
		sev_snapshot_mailbox_v2_shared_extents(mailbox),
		shared_extents_offset);
}

static void sev_snapshot_shared_init(void)
{
	struct sev_snapshot_mailbox *mailbox;
	bool do_seed = false;
	unsigned long flags;
	u64 start, end;

	raw_spin_lock_irqsave(&sev_snapshot_shared_lock, flags);

	if (sev_snapshot_shared_ready)
		goto out_unlock;

	mailbox = sev_snapshot_mailbox_va();
	if (!mailbox)
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
		unsigned long entry_size = entries[i].pagesize ? PMD_SIZE : PAGE_SIZE;

		if (!i) {
			range_start = gpa;
			next_gpa = gpa + entry_size;
			continue;
		}

		if (gpa == next_gpa) {
			next_gpa += entry_size;
			continue;
		}

		sev_snapshot_track_page_state_pa(range_start,
						 (next_gpa - range_start) >> PAGE_SHIFT,
						 op);
		range_start = gpa;
		next_gpa = gpa + entry_size;
	}

	sev_snapshot_track_page_state_pa(range_start,
					 (next_gpa - range_start) >> PAGE_SHIFT,
					 op);
}

static void sev_snapshot_fill_xsave(struct sev_snapshot_cpu_context *ctx)
{
	struct fpu *fpu;
	struct fpstate *fpstate;
	u32 len;

	fpregs_lock();
	fpu = this_cpu_read(fpu_fpregs_owner_ctx);
	if (fpu && fpu->fpstate) {
		save_fpregs_to_fpstate(fpu);
		fpstate = fpu->fpstate;
	} else {
		fpu = &current->thread.fpu;
		if (!test_thread_flag(TIF_NEED_FPU_LOAD))
			save_fpregs_to_fpstate(fpu);
		fpstate = fpu->fpstate;
	}
	if (!fpstate)
		goto out_unlock;

	ctx->xstate_bv = fpstate->regs.xsave.header.xfeatures;
	ctx->xsave_max_size = PAGE_SIZE;

	len = min_t(u32, fpstate->size, PAGE_SIZE);
	if (!len)
		goto out_unlock;

	ctx->xsave_len = len;
	memcpy(ctx->xsave, &fpstate->regs.xsave, len);
	ctx->valid_flags |= SEV_SNAPSHOT_CPU_CTX_F_XSAVE_DATA;
out_unlock:
	fpregs_unlock();
}

static void sev_snapshot_fill_context(struct sev_snapshot_cpu_context *ctx,
				      const struct pt_regs *regs,
				      bool capture_xsave)
{
	unsigned int cpu = smp_processor_id();

	if (cpu >= SEV_SNAPSHOT_V2_MAX_CPUS)
		sev_snapshot_restore_fault();

	memset(ctx, 0, sizeof(*ctx));

	ctx->context_size = sizeof(*ctx);
	ctx->regs = *regs;
	ctx->kernel_regs_ptr = (u64)regs;
	ctx->restore_stub_rip = (u64)sev_snapshot_restore_entry_early;
	ctx->restore_mailbox_va = (u64)sev_snapshot_restore_mailbox_va();

	savesegment(ds, ctx->ds);
	savesegment(es, ctx->es);
	savesegment(fs, ctx->fs);
	savesegment(gs, ctx->gs);
	store_ldt(ctx->ldt);
	store_tr(ctx->tr);
	ctx->tss = (u16)ctx->tr;
	ctx->valid_flags |= SEV_SNAPSHOT_CPU_CTX_F_SEGS |
			    SEV_SNAPSHOT_CPU_CTX_F_TR;

	rdmsrl(MSR_FS_BASE, ctx->fs_base);
	rdmsrl(MSR_GS_BASE, ctx->kernel_gs_base);
	rdmsrl(MSR_KERNEL_GS_BASE, ctx->user_gs_base);
	ctx->valid_flags |= SEV_SNAPSHOT_CPU_CTX_F_BASES;

	ctx->cr0 = read_cr0();
	ctx->cr2 = read_cr2();
	ctx->cr3 = __read_cr3();
	ctx->cr4 = __read_cr4();
	ctx->valid_flags |= SEV_SNAPSHOT_CPU_CTX_F_CONTROL;

	rdmsrl(MSR_EFER, ctx->efer);
	ctx->valid_flags |= SEV_SNAPSHOT_CPU_CTX_F_EFER;

	native_store_gdt(&ctx->gdt_desc);
	memcpy(ctx->gdt_copy, get_cpu_gdt_rw(cpu),
	       min_t(size_t, sizeof(ctx->gdt_copy),
		     (size_t)ctx->gdt_desc.size + 1));
	store_idt(&ctx->idt);
	ctx->valid_flags |= SEV_SNAPSHOT_CPU_CTX_F_TABLES;

	ctx->pkru = read_pkru();
	ctx->valid_flags |= SEV_SNAPSHOT_CPU_CTX_F_PKRU;
	ctx->xcr0 = xgetbv(XCR_XFEATURE_ENABLED_MASK);
	ctx->valid_flags |= SEV_SNAPSHOT_CPU_CTX_F_XSAVE_META;
	ctx->restore_gdt_page_va =
		(u64)sev_snapshot_restore_gdt_page_for_cpu(cpu);
	ctx->restore_gdt_page_pa =
		__pa_symbol(sev_snapshot_restore_gdt_pages) +
		(u64)cpu * PAGE_SIZE;
	ctx->restore_from_mailbox_rip = (u64)sev_snapshot_restore_from_mailbox;
#ifdef CONFIG_X86_64
	ctx->hardirq_stack_ptr = (u64)this_cpu_read(pcpu_hot.hardirq_stack_ptr);
#endif

	if (capture_xsave)
		sev_snapshot_fill_xsave(ctx);
}

static void __noreturn sev_snapshot_spin_after_capture(void)
{
	for (;;)
		asm volatile("pause" ::: "memory");
}

static void sev_snapshot_suspend_local_tick(void)
{
	unsigned long flags;

	local_irq_save(flags);
	tick_suspend_local();
	local_irq_restore(flags);
}

static int sev_snapshot_signal_host_capture(void)
{
	unsigned long flags;
	u64 ghcb_msr;

	local_irq_save(flags);
	ghcb_msr = __rdmsr(MSR_AMD64_SEV_ES_GHCB);
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB,
		      SEV_SNAPSHOT_REQUEST_HOST_EVENT_SIGNAL);
	VMGEXIT();
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB, ghcb_msr);
	local_irq_restore(flags);

	return 0;
}

static int sev_snapshot_prepare_capture(void)
{
	struct sev_snapshot_mailbox *mailbox;
	long ret;

	ret = sev_snapshot_map_transport_pages();
	if (ret)
		return ret;

	if (!sev_snapshot_mailbox_pa || !sev_snapshot_mailbox_len)
		return -ENODEV;

	sev_snapshot_shared_init();
	ret = sev_snapshot_encrypt_init_scratch_decrypted_pages();
	if (ret)
		return ret;

	mailbox = sev_snapshot_mailbox_va();
	if (!mailbox)
		return -ENODEV;

	return 0;
}

static bool sev_snapshot_multi_vcpu_restore_enabled(void)
{
	return sev_snapshot_restore_mode ==
	       SEV_SNAPSHOT_RESTORE_MODE_MULTI_VCPU;
}

static bool sev_snapshot_v2_cpu_count_valid(u32 cpu_count)
{
	return cpu_count >= SEV_SNAPSHOT_V2_MIN_CPUS &&
	       cpu_count <= SEV_SNAPSHOT_V2_MAX_CPUS;
}

static bool sev_snapshot_v2_online_cpus_match(u32 cpu_count)
{
	u32 cpu;

	if (!sev_snapshot_v2_cpu_count_valid(cpu_count))
		return false;
	if (num_online_cpus() != cpu_count)
		return false;

	for (cpu = 0; cpu < cpu_count; cpu++) {
		if (!cpu_online(cpu))
			return false;
	}

	return true;
}

static u32 sev_snapshot_restore_cpu_count(void)
{
	return sev_snapshot_multi_vcpu_restore_enabled() ?
		       num_online_cpus() :
		       SEV_SNAPSHOT_V2_MIN_CPUS;
}

static size_t sev_snapshot_mailbox_v2_topology_offset(void)
{
	return sizeof(struct sev_snapshot_mailbox_v2_header);
}

static size_t sev_snapshot_mailbox_v2_cpu_context_offset(u32 cpu_count)
{
	return ALIGN(sev_snapshot_mailbox_v2_topology_offset() +
			     sizeof(struct sev_snapshot_cpu_topology) * cpu_count,
		     8);
}

static size_t sev_snapshot_mailbox_v2_barrier_offset(u32 cpu_count)
{
	return sev_snapshot_mailbox_v2_cpu_context_offset(cpu_count) +
	       sizeof(struct sev_snapshot_cpu_context) * cpu_count;
}

static size_t sev_snapshot_mailbox_v2_cpu_bundle_len(u32 cpu_count)
{
	size_t len = sev_snapshot_mailbox_v2_barrier_offset(cpu_count);

	if (cpu_count > SEV_SNAPSHOT_V2_MIN_CPUS)
		len += sizeof(struct sev_snapshot_restore_barrier);

	return len;
}

static struct sev_snapshot_cpu_topology *
sev_snapshot_mailbox_v2_topology(struct sev_snapshot_mailbox_v2 *mailbox,
				 unsigned int slot)
{
	return (struct sev_snapshot_cpu_topology *)
		((u8 *)mailbox + sev_snapshot_mailbox_v2_topology_offset() +
		 slot * sizeof(struct sev_snapshot_cpu_topology));
}

static struct sev_snapshot_cpu_context *
sev_snapshot_mailbox_v2_cpu_context(struct sev_snapshot_mailbox_v2 *mailbox,
				    u32 cpu_count, unsigned int cpu)
{
	return (struct sev_snapshot_cpu_context *)
		((u8 *)mailbox +
		 sev_snapshot_mailbox_v2_cpu_context_offset(cpu_count) +
		 cpu * sizeof(struct sev_snapshot_cpu_context));
}

static struct sev_snapshot_restore_barrier *
sev_snapshot_mailbox_v2_restore_barrier(struct sev_snapshot_mailbox_v2 *mailbox)
{
	u32 cpu_count = READ_ONCE(mailbox->header.cpu_count);

	if (cpu_count <= SEV_SNAPSHOT_V2_MIN_CPUS ||
	    cpu_count > SEV_SNAPSHOT_V2_MAX_CPUS)
		sev_snapshot_restore_fault();

	return (struct sev_snapshot_restore_barrier *)
		((u8 *)mailbox +
		 sev_snapshot_mailbox_v2_barrier_offset(cpu_count));
}

static u32 *
sev_snapshot_mailbox_v2_shared_extent_count(struct sev_snapshot_mailbox_v2 *mailbox)
{
	return (u32 *)((u8 *)mailbox + READ_ONCE(mailbox->header.cpu_bundle_len));
}

static struct sev_snapshot_shared_extent *
sev_snapshot_mailbox_v2_shared_extents(struct sev_snapshot_mailbox_v2 *mailbox)
{
	return (struct sev_snapshot_shared_extent *)
		((u8 *)sev_snapshot_mailbox_v2_shared_extent_count(mailbox) +
		 2 * sizeof(u32));
}

static void sev_snapshot_fill_topology(struct sev_snapshot_mailbox_v2 *mailbox,
				       u32 cpu_count,
				       unsigned int slot,
				       unsigned int cpu)
{
	struct sev_snapshot_cpu_topology *topology;

	if (!mailbox || slot >= cpu_count)
		return;

	topology = sev_snapshot_mailbox_v2_topology(mailbox, slot);
	topology->linux_cpu_id = cpu;
	topology->apic_id = cpu_physical_id(cpu);
	topology->vcpu_index = cpu;
}

static void sev_snapshot_init_mailbox_v2(struct sev_snapshot_mailbox_v2 *mailbox,
					 u32 cpu_count)
{
	size_t bundle_len = sev_snapshot_mailbox_v2_cpu_bundle_len(cpu_count);

	memset(mailbox, 0, bundle_len + 2 * sizeof(u32));
	mailbox->header.magic = SEV_SNAPSHOT_MAILBOX_V2_MAGIC;
	mailbox->header.version = SEV_SNAPSHOT_MAILBOX_V2_VERSION;
	mailbox->header.header_size =
		sizeof(struct sev_snapshot_mailbox_v2_header);
	mailbox->header.mode = sev_snapshot_restore_mode;
	mailbox->header.cpu_count = cpu_count;
	mailbox->header.cpu_bundle_len = bundle_len;
}

static __always_inline void
sev_snapshot_store_current_kernel_ptregs(struct pt_regs *regs,
					 unsigned long ip)
{
	memset(regs, 0, sizeof(*regs));

	asm volatile("movq %%r15, %c[r15](%[regs])\n\t"
		     "movq %%r14, %c[r14](%[regs])\n\t"
		     "movq %%r13, %c[r13](%[regs])\n\t"
		     "movq %%r12, %c[r12](%[regs])\n\t"
		     "movq %%rbp, %c[bp](%[regs])\n\t"
		     "movq %%rbx, %c[bx](%[regs])\n\t"
		     "movq %%r11, %c[r11](%[regs])\n\t"
		     "movq %%r10, %c[r10](%[regs])\n\t"
		     "movq %%r9, %c[r9](%[regs])\n\t"
		     "movq %%r8, %c[r8](%[regs])\n\t"
		     "movq %%rax, %c[ax](%[regs])\n\t"
		     "movq %%rcx, %c[cx](%[regs])\n\t"
		     "movq %%rdx, %c[dx](%[regs])\n\t"
		     "movq %%rsi, %c[si](%[regs])\n\t"
		     "movq %%rdi, %c[di](%[regs])\n\t"
		     "movq %%rsp, %c[sp](%[regs])\n\t"
		     "pushfq\n\t"
		     "popq %%rax\n\t"
		     "movq %%rax, %c[flags](%[regs])\n\t"
		     :
		     : [regs] "r" (regs),
		       [r15] "i" (offsetof(struct pt_regs, r15)),
		       [r14] "i" (offsetof(struct pt_regs, r14)),
		       [r13] "i" (offsetof(struct pt_regs, r13)),
		       [r12] "i" (offsetof(struct pt_regs, r12)),
		       [bp] "i" (offsetof(struct pt_regs, bp)),
		       [bx] "i" (offsetof(struct pt_regs, bx)),
		       [r11] "i" (offsetof(struct pt_regs, r11)),
		       [r10] "i" (offsetof(struct pt_regs, r10)),
		       [r9] "i" (offsetof(struct pt_regs, r9)),
		       [r8] "i" (offsetof(struct pt_regs, r8)),
		       [ax] "i" (offsetof(struct pt_regs, ax)),
		       [cx] "i" (offsetof(struct pt_regs, cx)),
		       [dx] "i" (offsetof(struct pt_regs, dx)),
		       [si] "i" (offsetof(struct pt_regs, si)),
		       [di] "i" (offsetof(struct pt_regs, di)),
		       [sp] "i" (offsetof(struct pt_regs, sp)),
		       [flags] "i" (offsetof(struct pt_regs, flags))
		     : "rax", "memory");

	regs->orig_ax = -1;
	regs->ip = ip;
	regs->cs = __KERNEL_CS;
	regs->flags &= ~X86_EFLAGS_TF;
	regs->ss = __KERNEL_DS;
}

static void sev_snapshot_capture_secondary_cpu_v2(void)
{
	struct sev_snapshot_mailbox_v2 *mailbox =
		READ_ONCE(sev_snapshot_v2_ap_mailbox);
	struct pt_regs *regs;
	u32 cpu_count = READ_ONCE(sev_snapshot_v2_ap_cpu_count);
	unsigned int cpu = raw_smp_processor_id();
	unsigned long flags;

	if (!mailbox || !sev_snapshot_v2_cpu_count_valid(cpu_count) ||
	    cpu == 0 || cpu >= cpu_count) {
		pr_warn("multi-vCPU AP rendezvous capture failed: cpu=%u cpu_count=%u mailbox=%px\n",
			cpu, cpu_count, mailbox);
		atomic_set(&sev_snapshot_v2_ap_error, 1);
		atomic_inc(&sev_snapshot_v2_ap_ready);
		return;
	}

	regs = &sev_snapshot_v2_ap_regs[cpu];
	preempt_disable();
	/*
	 * This rendezvous runs from a kernel thread.  If the scheduler left the
	 * CPU in lazy TLB mode, CR3 can still point at a previous user mm whose
	 * KPTI kernel view does not map the restore bridge area.  Force and
	 * record a canonical init_mm kernel CR3 for the AP checkpoint.
	 */
	leave_mm(cpu);
	initialize_tlbstate_and_flush();

	local_irq_save(flags);
	sev_snapshot_store_current_kernel_ptregs(regs,
						 (unsigned long)&&restore_continue);

	sev_snapshot_fill_topology(mailbox, cpu_count, cpu, cpu);
	/*
	 * APs checkpoint from cooperative kernel threads. Capture the
	 * hardware XSAVE image as part of the guest-owned context so the runtime
	 * can validate and replay the AP restore state instead of synthesizing it.
	 */
	sev_snapshot_fill_context(
		sev_snapshot_mailbox_v2_cpu_context(mailbox, cpu_count, cpu),
		regs, true);
	sev_snapshot_suspend_local_tick();
	smp_wmb();
	atomic_inc(&sev_snapshot_v2_ap_ready);

	sev_snapshot_signal_host_capture();
	while (!READ_ONCE(sev_snapshot_v2_ap_restore_escape))
		asm volatile("pause" ::: "memory");

restore_continue:
	if (sev_snapshot_multi_vcpu_restore_enabled())
		sev_snapshot_restore_v2_ap_continue_after_ptregs();
	sev_snapshot_restore_debug_code(0xec);
	preempt_enable_no_resched();
	local_irq_restore(flags);
	if (sev_snapshot_multi_vcpu_restore_enabled())
		sev_snapshot_restore_v2_ap_wait_after_context_restore();
	sev_snapshot_restore_debug_code(0xeb);
}

static int sev_snapshot_v2_ap_thread_fn(void *unused)
{
	int seen_epoch = atomic_read(&sev_snapshot_v2_ap_request_epoch);

	while (!kthread_should_stop()) {
		wait_event_interruptible(
			sev_snapshot_v2_ap_waitq,
			kthread_should_stop() ||
			 atomic_read(&sev_snapshot_v2_ap_request_epoch) !=
				 seen_epoch);
		if (kthread_should_stop())
			break;
		if (atomic_read(&sev_snapshot_v2_ap_request_epoch) == seen_epoch)
			continue;
		seen_epoch = atomic_read(&sev_snapshot_v2_ap_request_epoch);
		sev_snapshot_capture_secondary_cpu_v2();
	}

	return 0;
}

static int sev_snapshot_v2_ensure_ap_thread(unsigned int cpu)
{
	struct task_struct *task;
	int ret = 0;

	if (cpu == 0 || cpu >= SEV_SNAPSHOT_V2_MAX_CPUS || !cpu_online(cpu))
		return -EINVAL;

	if (READ_ONCE(sev_snapshot_v2_ap_tasks[cpu]))
		return 0;

	mutex_lock(&sev_snapshot_v2_ap_task_lock);
	if (READ_ONCE(sev_snapshot_v2_ap_tasks[cpu]))
		goto out;

	task = kthread_create_on_cpu(sev_snapshot_v2_ap_thread_fn, NULL, cpu,
				     "sev_snapshot_v2_ap");
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		goto out;
	}

	WRITE_ONCE(sev_snapshot_v2_ap_tasks[cpu], task);
	wake_up_process(task);

out:
	mutex_unlock(&sev_snapshot_v2_ap_task_lock);
	return ret;
}

static int sev_snapshot_v2_ensure_ap_threads(u32 cpu_count)
{
	u32 cpu;
	int ret;

	if (!sev_snapshot_v2_online_cpus_match(cpu_count) ||
	    cpu_count <= SEV_SNAPSHOT_V2_MIN_CPUS)
		return -EINVAL;

	for (cpu = 1; cpu < cpu_count; cpu++) {
		ret = sev_snapshot_v2_ensure_ap_thread(cpu);
		if (ret)
			return ret;
	}

	return 0;
}

static long sev_snapshot_rendezvous_secondary_cpu_v2(
	struct sev_snapshot_mailbox_v2 *mailbox, u32 cpu_count)
{
	int expected_ready = (int)cpu_count - 1;
	u32 cpu;
	unsigned int wait;

	if (!mailbox || !sev_snapshot_v2_online_cpus_match(cpu_count) ||
	    cpu_count <= SEV_SNAPSHOT_V2_MIN_CPUS)
		return -EINVAL;

	for (cpu = 1; cpu < cpu_count; cpu++) {
		if (!READ_ONCE(sev_snapshot_v2_ap_tasks[cpu]))
			return -ENODEV;
	}

	atomic_set(&sev_snapshot_v2_ap_ready, 0);
	atomic_set(&sev_snapshot_v2_ap_error, 0);
	WRITE_ONCE(sev_snapshot_v2_ap_cpu_count, cpu_count);
	WRITE_ONCE(sev_snapshot_v2_ap_mailbox, mailbox);
	smp_wmb();

	atomic_inc(&sev_snapshot_v2_ap_request_epoch);
	wake_up_interruptible(&sev_snapshot_v2_ap_waitq);
	for (cpu = 1; cpu < cpu_count; cpu++)
		wake_up_process(READ_ONCE(sev_snapshot_v2_ap_tasks[cpu]));

	for (wait = 0; wait < 5000; wait++) {
		if (atomic_read(&sev_snapshot_v2_ap_ready) >= expected_ready)
			break;
		usleep_range(100, 200);
	}
	if (atomic_read(&sev_snapshot_v2_ap_ready) < expected_ready) {
		WRITE_ONCE(sev_snapshot_v2_ap_mailbox, NULL);
		WRITE_ONCE(sev_snapshot_v2_ap_cpu_count, 0);
		return -ETIMEDOUT;
	}
	if (atomic_read(&sev_snapshot_v2_ap_error)) {
		WRITE_ONCE(sev_snapshot_v2_ap_mailbox, NULL);
		WRITE_ONCE(sev_snapshot_v2_ap_cpu_count, 0);
		return -EIO;
	}

	return 0;
}

static long sev_snapshot_capture_final(const struct pt_regs *regs)
{
	struct sev_snapshot_mailbox *mailbox;
	struct sev_snapshot_mailbox_v2 *mailbox_v2;
	u32 cpu_count;
	unsigned int cpu;
	long ret;

	if (!regs)
		return -EINVAL;

	mailbox = sev_snapshot_mailbox_va();
	if (!mailbox)
		return -ENODEV;

	cpu = smp_processor_id();
	if (cpu)
		return -EINVAL;

	cpu_count = sev_snapshot_restore_cpu_count();
	if (!sev_snapshot_v2_online_cpus_match(cpu_count) ||
	    (sev_snapshot_multi_vcpu_restore_enabled() &&
	     cpu_count <= SEV_SNAPSHOT_V2_MIN_CPUS))
		return -EINVAL;

	mailbox_v2 = (struct sev_snapshot_mailbox_v2 *)mailbox;
	sev_snapshot_init_mailbox_v2(mailbox_v2, cpu_count);
	ret = sev_snapshot_freeze_virtio_devices();
	if (ret)
		return ret;
	if (cpu_count > SEV_SNAPSHOT_V2_MIN_CPUS) {
		ret = sev_snapshot_rendezvous_secondary_cpu_v2(mailbox_v2,
							       cpu_count);
		if (ret)
			return ret;
	}

	sev_snapshot_fill_topology(mailbox_v2, cpu_count, 0, cpu);
	sev_snapshot_fill_context(
		sev_snapshot_mailbox_v2_cpu_context(mailbox_v2, cpu_count, 0),
		regs, true);
	sev_snapshot_suspend_local_tick();
	sev_snapshot_rebuild_shared_extents_from_direct_map();
	sev_snapshot_flush_shared_extents_v2(mailbox_v2);
	clflush_cache_range(mailbox_v2, SEV_SNAPSHOT_MAILBOX_SIZE);
	wmb();
	ret = sev_snapshot_signal_host_capture();
	if (ret)
		return ret;

	sev_snapshot_spin_after_capture();
}

long sev_snapshot_arm(void)
{
	u32 cpu_count;
	long ret;

	ret = sev_snapshot_pin_current_to_boot_cpu();
	if (ret) {
		sev_snapshot_abort_capture_setup();
		return ret;
	}

	cpu_count = sev_snapshot_restore_cpu_count();
	if (!sev_snapshot_multi_vcpu_restore_enabled()) {
		if (!sev_snapshot_v2_online_cpus_match(cpu_count)) {
			sev_snapshot_abort_capture_setup();
			return -EINVAL;
		}
	} else {
		if (!sev_snapshot_v2_online_cpus_match(cpu_count) ||
		    cpu_count <= SEV_SNAPSHOT_V2_MIN_CPUS) {
			sev_snapshot_abort_capture_setup();
			return -EINVAL;
		}
		ret = sev_snapshot_v2_ensure_ap_threads(cpu_count);
		if (ret) {
			sev_snapshot_abort_capture_setup();
			return ret;
		}
	}

	ret = sev_snapshot_prepare_capture();
	if (ret) {
		sev_snapshot_abort_capture_setup();
		return ret;
	}

	this_cpu_write(sev_snapshot_pending_task, current);
	return 0;
}

void sev_snapshot_finalize_user_return(struct pt_regs *regs)
{
	struct task_struct *pending;
	long ret;

	if (!regs)
		return;

	pending = this_cpu_read(sev_snapshot_pending_task);
	if (pending != current)
		return;

	this_cpu_write(sev_snapshot_pending_task, NULL);

	if (regs->orig_ax != __NR_snapshot_enter) {
		sev_snapshot_abort_capture_setup();
		return;
	}

	if (regs->ax < 0) {
		sev_snapshot_abort_capture_setup();
		return;
	}

	ret = sev_snapshot_capture_final(regs);
	if (ret) {
		sev_snapshot_abort_capture_setup();
		regs->ax = ret;
	}
}

static __always_inline void __noreturn sev_snapshot_restore_fault(void)
{
	BUG();
	unreachable();
}

static __always_inline void sev_snapshot_restore_copy(void *dst, const void *src,
						     size_t len)
{
	u8 *d = dst;
	const u8 *s = src;
	size_t i;

	for (i = 0; i < len; i++)
		WRITE_ONCE(d[i], READ_ONCE(s[i]));
}

static void *sev_snapshot_restore_xsave_scratch(struct sev_snapshot_mailbox *mailbox,
						u64 restore_cpu_id)
{
	unsigned long offset;

	if (!sev_snapshot_mailbox_is_v2(mailbox))
		return sev_snapshot_restore_xsave;

	offset = ALIGN(((struct sev_snapshot_mailbox_v2 *)mailbox)
			       ->header.cpu_bundle_len +
		       2 * sizeof(u32),
		       64);
	offset += restore_cpu_id * PAGE_SIZE;
	if (offset + PAGE_SIZE > SEV_SNAPSHOT_MAILBOX_SIZE)
		sev_snapshot_restore_fault();

	return (u8 *)mailbox + offset;
}

static void sev_snapshot_restore_xsave_state(
	struct sev_snapshot_mailbox *mailbox,
	const struct sev_snapshot_cpu_context *ctx,
	u64 restore_cpu_id)
{
	void *scratch;

	if (ctx->valid_flags & SEV_SNAPSHOT_CPU_CTX_F_XSAVE_META)
		xsetbv(XCR_XFEATURE_ENABLED_MASK, ctx->xcr0);

	if (!(ctx->valid_flags & SEV_SNAPSHOT_CPU_CTX_F_XSAVE_DATA) ||
	    !ctx->xsave_len)
		sev_snapshot_restore_fault();

	scratch = sev_snapshot_restore_xsave_scratch(mailbox, restore_cpu_id);
	memset(scratch, 0, PAGE_SIZE);
	memcpy(scratch, ctx->xsave, ctx->xsave_len);

	asm volatile("xrstor (%[scratch])"
			 :
			 : [scratch] "r" (scratch),
		       "a" ((u32)ctx->xstate_bv),
		       "d" ((u32)(ctx->xstate_bv >> 32))
		     : "memory");
}

static __always_inline struct x86_hw_tss *
sev_snapshot_restore_tss_va(unsigned int cpu)
{
	unsigned long cea = CPU_ENTRY_AREA_PER_CPU +
			    (unsigned long)cpu * CPU_ENTRY_AREA_SIZE;

	return &((struct cpu_entry_area *)cea)->tss.x86_tss;
}

static void sev_snapshot_prepare_restore_gdt(
	const struct sev_snapshot_cpu_context *ctx,
	struct desc_ptr *gdt_desc,
	u64 restore_cpu_id)
{
	struct ldttss_desc *tss;
	struct x86_hw_tss *live_tss;
	size_t gdt_size, tss_offset;
	void *gdt_base;

	gdt_size = (size_t)ctx->gdt_desc.size + 1;
	tss_offset = (size_t)(ctx->tss & ~0x7);

	if (!ctx->gdt_desc.address || !gdt_size)
		sev_snapshot_restore_fault();

	if (tss_offset + sizeof(*tss) > gdt_size)
		sev_snapshot_restore_fault();

	if (gdt_size > sizeof(ctx->gdt_copy) || gdt_size > PAGE_SIZE)
		sev_snapshot_restore_fault();

	gdt_base = sev_snapshot_restore_gdt_page_for_cpu(restore_cpu_id);
	sev_snapshot_restore_copy(gdt_base, ctx->gdt_copy, gdt_size);

	tss = (struct ldttss_desc *)((u8 *)gdt_base + tss_offset);

	/*
	 * Keep the final GDTR on a kernel-owned page that remains mapped after
	 * the restore bridge is no longer needed.  The mailbox copy is only the
	 * source for the snapshot GDT contents.
	 */
	live_tss = sev_snapshot_restore_tss_va(restore_cpu_id);
	set_tssldt_descriptor(tss, (unsigned long)live_tss, DESC_TSS,
			      __KERNEL_TSS_LIMIT);

	gdt_desc->size = ctx->gdt_desc.size;
	gdt_desc->address = (unsigned long)gdt_base;
}

static void sev_snapshot_restore_tr_from_gdt(const struct desc_ptr *gdt_desc,
					     u16 restore_tss)
{
	if (!gdt_desc)
		sev_snapshot_restore_fault();

	asm volatile("ltr %0" : : "m" (restore_tss));
}

static __always_inline void sev_snapshot_reload_kernel_cs(void)
{
	asm volatile("pushq %[cs]\n\t"
		     "leaq 1f(%%rip), %%rax\n\t"
		     "pushq %%rax\n\t"
		     "lretq\n"
		     "1:"
		     :
		     : [cs] "i" (__KERNEL_CS)
		     : "rax", "memory");
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

static bool sev_snapshot_mailbox_is_v2(struct sev_snapshot_mailbox *mailbox)
{
	struct sev_snapshot_mailbox_v2 *mailbox_v2 =
		(struct sev_snapshot_mailbox_v2 *)mailbox;

	return READ_ONCE(mailbox_v2->header.magic) ==
		       SEV_SNAPSHOT_MAILBOX_V2_MAGIC &&
	       READ_ONCE(mailbox_v2->header.version) ==
		       SEV_SNAPSHOT_MAILBOX_V2_VERSION;
}

static struct sev_snapshot_cpu_context *sev_snapshot_select_restore_context(
	struct sev_snapshot_mailbox *mailbox,
	u64 restore_cpu_id)
{
	struct sev_snapshot_mailbox_v2 *mailbox_v2;
	struct sev_snapshot_cpu_topology *topology;
	u32 cpu_count;

	if (!mailbox)
		sev_snapshot_restore_fault();

	if (!sev_snapshot_mailbox_is_v2(mailbox))
		sev_snapshot_restore_fault();

	mailbox_v2 = (struct sev_snapshot_mailbox_v2 *)mailbox;
	cpu_count = READ_ONCE(mailbox_v2->header.cpu_count);
	if (cpu_count < SEV_SNAPSHOT_V2_MIN_CPUS ||
	    cpu_count > SEV_SNAPSHOT_V2_MAX_CPUS)
		sev_snapshot_restore_fault();
	if (cpu_count == SEV_SNAPSHOT_V2_MIN_CPUS &&
	    READ_ONCE(mailbox_v2->header.mode) !=
		    SEV_SNAPSHOT_RESTORE_MODE_SINGLE_VCPU)
		sev_snapshot_restore_fault();
	if (cpu_count > SEV_SNAPSHOT_V2_MIN_CPUS &&
	    READ_ONCE(mailbox_v2->header.mode) !=
		    SEV_SNAPSHOT_RESTORE_MODE_MULTI_VCPU)
		sev_snapshot_restore_fault();
	if (restore_cpu_id >= cpu_count)
		sev_snapshot_restore_fault();

	topology = sev_snapshot_mailbox_v2_topology(mailbox_v2, restore_cpu_id);
	if (READ_ONCE(topology->linux_cpu_id) != restore_cpu_id ||
	    READ_ONCE(topology->vcpu_index) != restore_cpu_id ||
	    READ_ONCE(topology->apic_id) != restore_cpu_id)
		sev_snapshot_restore_fault();

	return sev_snapshot_mailbox_v2_cpu_context(mailbox_v2, cpu_count,
						   restore_cpu_id);
}

static struct sev_snapshot_restore_barrier *
sev_snapshot_restore_v2_barrier(struct sev_snapshot_mailbox *mailbox)
{
	if (!sev_snapshot_mailbox_is_v2(mailbox))
		sev_snapshot_restore_fault();

	return sev_snapshot_mailbox_v2_restore_barrier(
		(struct sev_snapshot_mailbox_v2 *)mailbox);
}

static u32 sev_snapshot_restore_v2_cpu_count(struct sev_snapshot_mailbox *mailbox)
{
	struct sev_snapshot_mailbox_v2 *mailbox_v2;
	u32 cpu_count;

	if (!sev_snapshot_mailbox_is_v2(mailbox))
		sev_snapshot_restore_fault();

	mailbox_v2 = (struct sev_snapshot_mailbox_v2 *)mailbox;
	cpu_count = READ_ONCE(mailbox_v2->header.cpu_count);
	if (cpu_count <= SEV_SNAPSHOT_V2_MIN_CPUS ||
	    cpu_count > SEV_SNAPSHOT_V2_MAX_CPUS)
		sev_snapshot_restore_fault();

	return cpu_count;
}

static int sev_snapshot_restore_barrier_read(u32 *field)
{
	return atomic_read((atomic_t *)field);
}

static void sev_snapshot_restore_barrier_set(u32 *field, int value)
{
	atomic_set((atomic_t *)field, value);
	smp_mb();
}

static void sev_snapshot_restore_barrier_inc(u32 *field)
{
	atomic_inc((atomic_t *)field);
	smp_mb__after_atomic();
}

static void sev_snapshot_restore_barrier_wait_at_least(u32 *field, int value)
{
	while (sev_snapshot_restore_barrier_read(field) < value) {
		smp_rmb();
		barrier();
	}
}

static void sev_snapshot_restore_barrier_wait_nonzero_irq_poll(u32 *field)
{
	while (!sev_snapshot_restore_barrier_read(field)) {
		asm volatile("pause" ::: "memory");
		smp_rmb();
		barrier();
	}
}

static void sev_snapshot_restore_v2_ap_continue_after_ptregs(void)
{
	struct sev_snapshot_mailbox *mailbox = sev_snapshot_restore_mailbox_va();
	struct sev_snapshot_restore_barrier *barrier =
		sev_snapshot_restore_v2_barrier(mailbox);
	u32 cpu = raw_smp_processor_id();

	sev_snapshot_restore_debug_code(0xe1);
	kvm_sev_snapshot_restore_reinit_ap();
	sev_snapshot_restore_debug_code(0xe3);
	sev_snapshot_resume_local_tick();
	sev_snapshot_rearm_boot_local_apic_timer();
	sev_snapshot_restore_debug_code(0xe5);

	sev_snapshot_restore_barrier_set(&barrier->ap_continue, cpu + 1);
	sev_snapshot_restore_barrier_inc(&barrier->ap_continue_ready);
}

static void sev_snapshot_restore_v2_ap_wait_after_context_restore(void)
{
	struct sev_snapshot_mailbox *mailbox = sev_snapshot_restore_mailbox_va();
	struct sev_snapshot_restore_barrier *barrier =
		sev_snapshot_restore_v2_barrier(mailbox);
	u32 cpu_count = sev_snapshot_restore_v2_cpu_count(mailbox);

	sev_snapshot_restore_debug_code(0xe7);

	sev_snapshot_restore_barrier_inc(&barrier->resume_ready);
	sev_snapshot_restore_barrier_wait_nonzero_irq_poll(&barrier->virtio_done);
	sev_snapshot_restore_barrier_wait_at_least(&barrier->resume_ready,
						   cpu_count);
	sev_snapshot_restore_debug_code(0xe9);
}

static void __noreturn sev_snapshot_restore_v2_ap_handoff_to_continuation(
	struct sev_snapshot_mailbox *mailbox,
	const struct sev_snapshot_cpu_context *ctx,
	u64 restore_cpu_id)
{
	struct sev_snapshot_restore_barrier *barrier =
		sev_snapshot_restore_v2_barrier(mailbox);
	u32 cpu_count = sev_snapshot_restore_v2_cpu_count(mailbox);

	sev_snapshot_restore_debug_code(0xa5);
	sev_snapshot_restore_barrier_inc(&barrier->ready);
	sev_snapshot_restore_barrier_wait_at_least(&barrier->ready,
						   cpu_count);
	sev_snapshot_restore_debug_code(0xa7);

	sev_snapshot_restore_barrier_wait_at_least(&barrier->ap_continue,
						   restore_cpu_id);
	sev_snapshot_restore_debug_code(0xad);

	local_irq_disable();
	sev_snapshot_leave_irq_stack_restore_window(ctx, restore_cpu_id);
	sev_snapshot_resume_kernel_thread_from_ptregs(&ctx->regs);
	unreachable();
}

static void sev_snapshot_restore_v2_bsp_barrier_before_resume(
	struct sev_snapshot_mailbox *mailbox)
{
	struct sev_snapshot_restore_barrier *barrier =
		sev_snapshot_restore_v2_barrier(mailbox);
	u32 cpu_count = sev_snapshot_restore_v2_cpu_count(mailbox);
	int ret;

	sev_snapshot_restore_barrier_inc(&barrier->ready);
	sev_snapshot_restore_barrier_wait_at_least(&barrier->ready,
						   cpu_count);

	sev_snapshot_restore_debug_code(0xe0);
	sev_snapshot_restore_barrier_set(&barrier->ap_continue, 1);
	sev_snapshot_restore_debug_code(0xe2);
	sev_snapshot_restore_barrier_wait_at_least(&barrier->ap_continue_ready,
						   cpu_count - 1);
	sev_snapshot_restore_debug_code(0xe4);

	sev_snapshot_restore_debug_code(0xe6);
	ret = sev_snapshot_restore_virtio_devices();
	if (ret)
		sev_snapshot_restore_fault();
	sev_snapshot_restore_debug_code(0xe8);
	sev_snapshot_restore_barrier_set(&barrier->virtio_done, 1);
	sev_snapshot_restore_debug_code(0xea);

	sev_snapshot_restore_barrier_inc(&barrier->resume_ready);
	sev_snapshot_restore_barrier_wait_at_least(&barrier->resume_ready,
						   cpu_count);
	sev_snapshot_resume_local_tick();
	sev_snapshot_restore_debug_code(0xed);
	sev_snapshot_rearm_boot_local_apic_timer();
	sev_snapshot_restore_debug_code(0xee);
}

void __noreturn sev_snapshot_restore_from_mailbox(
	struct sev_snapshot_mailbox *mailbox,
	u64 restore_cpu_id)
{
	struct sev_snapshot_cpu_context *ctx;
	struct sev_snapshot_mailbox_v2 *mailbox_v2;
	struct desc_ptr restore_gdt_desc;
	struct pt_regs *regs;
	u32 restore_cpu_count = 1;
	volatile u16 restore_ds, restore_es, restore_fs, restore_gs, restore_tss;

	ctx = sev_snapshot_select_restore_context(mailbox, restore_cpu_id);
	if (sev_snapshot_mailbox_is_v2(mailbox)) {
		mailbox_v2 = (struct sev_snapshot_mailbox_v2 *)mailbox;
		restore_cpu_count = READ_ONCE(mailbox_v2->header.cpu_count);
	}
	sev_snapshot_restore_cpu_debug_code(0xd0, restore_cpu_id);
	restore_ds = READ_ONCE(ctx->ds);
	restore_es = READ_ONCE(ctx->es);
	restore_fs = READ_ONCE(ctx->fs);
	restore_gs = READ_ONCE(ctx->gs);
	restore_tss = READ_ONCE(ctx->tss);

	sev_snapshot_prepare_restore_gdt(ctx, &restore_gdt_desc, restore_cpu_id);
	load_gdt(&restore_gdt_desc);
	sev_snapshot_reload_kernel_cs();
	asm volatile("movw %[ds], %%ss\n\t"
		     "movw %[ds], %%ds\n\t"
		     "movw %[ds], %%es\n\t"
		     :
			     : [ds] "r" ((__u16)__KERNEL_DS)
			     : "memory");

	load_idt(&ctx->idt);
	sev_snapshot_restore_tr_from_gdt(&restore_gdt_desc, restore_tss);
	sev_snapshot_restore_cpu_debug_code(0xd2, restore_cpu_id);

	regs = (struct pt_regs *)(unsigned long)ctx->kernel_regs_ptr;
	sev_snapshot_restore_copy(regs, &ctx->regs, sizeof(*regs));
	regs->flags &= ~X86_EFLAGS_TF;
	if (user_mode(regs))
		regs->flags |= X86_EFLAGS_IF;
	sev_snapshot_restore_cpu_debug_code(0xd8, restore_cpu_id);

	write_cr0(ctx->cr0);
	write_cr2(ctx->cr2);
	wrmsrl(MSR_EFER, ctx->efer);
	sev_snapshot_restore_cpu_debug_code(0xc3, restore_cpu_id);

	sev_snapshot_register_runtime_ghcb_cpu(restore_cpu_id);
	sev_snapshot_restore_cpu_debug_code(0xc5, restore_cpu_id);
	if (!sev_snapshot_negotiate_runtime_ghcb_protocol())
		sev_snapshot_restore_fault();
	sev_snapshot_restore_cpu_debug_code(0xc7, restore_cpu_id);

	wrmsrl(MSR_FS_BASE, ctx->fs_base);
	wrmsrl(MSR_GS_BASE, ctx->kernel_gs_base);
	wrmsrl(MSR_KERNEL_GS_BASE, ctx->user_gs_base);
	sev_snapshot_restore_cpu_debug_code(0xc9, restore_cpu_id);
	native_write_cr4(ctx->cr4);
	sev_snapshot_restore_cpu_debug_code(0xb4, restore_cpu_id);
	sev_snapshot_enter_irq_stack_restore_window(ctx, restore_cpu_id);
	sev_snapshot_restore_cpu_debug_code(0xcb, restore_cpu_id);
	syscall_init();
	write_pkru(ctx->pkru);
	sev_snapshot_restore_cpu_debug_code(0xcd, restore_cpu_id);

	sev_snapshot_restore_xsave_state(mailbox, ctx, restore_cpu_id);
	if (sev_snapshot_mailbox_is_v2(mailbox))
		sev_snapshot_restore_cpu_debug_code(0xce, restore_cpu_id);

	sev_snapshot_install_sp0_scratch_stack();
	if (sev_snapshot_mailbox_is_v2(mailbox))
		sev_snapshot_restore_cpu_debug_code(0xa8, restore_cpu_id);

	if (restore_cpu_id == 0) {
		if (sev_snapshot_mailbox_is_v2(mailbox))
			sev_snapshot_restore_debug_code(0xaa);
		kvm_sev_snapshot_restore_reinit();
		if (sev_snapshot_mailbox_is_v2(mailbox))
			sev_snapshot_restore_debug_code(0xab);
	}
	if (restore_cpu_id == 0) {
		sev_snapshot_drain_local_apic_isr();
		sev_snapshot_clear_idle_polling_flags();
		if (sev_snapshot_mailbox_is_v2(mailbox))
			sev_snapshot_restore_debug_code(0xb0);
		if (!sev_snapshot_mailbox_is_v2(mailbox) ||
		    restore_cpu_count == SEV_SNAPSHOT_V2_MIN_CPUS) {
			sev_snapshot_resume_local_tick();
			sev_snapshot_rearm_boot_local_apic_timer();
		}
	}
	if (sev_snapshot_mailbox_is_v2(mailbox) &&
	    restore_cpu_count > SEV_SNAPSHOT_V2_MIN_CPUS) {
		if (restore_cpu_id == 0)
			sev_snapshot_restore_v2_bsp_barrier_before_resume(mailbox);
			else
				sev_snapshot_restore_v2_ap_handoff_to_continuation(
					mailbox, ctx, restore_cpu_id);
	} else if (sev_snapshot_restore_virtio_devices()) {
		sev_snapshot_restore_fault();
	}

	if (sev_snapshot_mailbox_is_v2(mailbox))
		sev_snapshot_restore_debug_code(0xef);
	local_irq_disable();
	sev_snapshot_leave_irq_stack_restore_window(ctx, restore_cpu_id);
	sev_snapshot_resume_from_ptregs(regs);
	unreachable();
}

void __init sev_mailbox_reserve_from_setup_data(struct boot_params *bp)
{
	u64 pa_data;

	if (!bp)
		return;

	pa_data = bp->hdr.setup_data;
	while (pa_data) {
		struct setup_data *hdr;
		struct sev_snapshot_mailbox_desc desc;
		u64 next;
		u32 len;

		hdr = early_memremap(pa_data, sizeof(*hdr));
		if (!hdr) {
			pr_err("failed to map setup_data while scanning for SEV snapshot mailbox\n");
			return;
		}

		next = hdr->next;
		len = hdr->len;

		if (hdr->type != SETUP_SEV_SNAPSHOT_MAILBOX) {
			early_memunmap(hdr, sizeof(*hdr));
			pa_data = next;
			continue;
		}

		if (len < sizeof(desc)) {
			early_memunmap(hdr, sizeof(*hdr));
			pr_err("ignoring short SEV snapshot mailbox descriptor\n");
			return;
		}

		early_memunmap(hdr, sizeof(*hdr));

		hdr = early_memremap(pa_data, sizeof(*hdr) + len);
		if (!hdr) {
			pr_err("failed to remap SEV snapshot mailbox descriptor\n");
			return;
		}

		memcpy(&desc, hdr->data, sizeof(desc));
		early_memunmap(hdr, sizeof(*hdr) + len);

		if (!desc.phys_addr || desc.size != SEV_SNAPSHOT_MAILBOX_SIZE ||
		    (desc.phys_addr & (SEV_SNAPSHOT_MAILBOX_SIZE - 1)) ||
		    desc.phys_addr == (~(u64)(SEV_SNAPSHOT_MAILBOX_SIZE - 1))) {
			pr_err("ignoring SEV snapshot mailbox descriptor with invalid address/size\n");
			return;
		}

		if (!sev_snapshot_range_is_ram(desc.phys_addr, desc.size))
			pr_warn("SEV snapshot mailbox backing is not visible yet during early setup; reserving descriptor range anyway\n");

		if (memblock_is_region_reserved(desc.phys_addr, desc.size)) {
			pr_err("SEV snapshot mailbox overlaps an already reserved range\n");
			return;
		}

		if (memblock_reserve(desc.phys_addr, desc.size)) {
			pr_err("failed to reserve SEV snapshot mailbox range\n");
			return;
		}

		sev_snapshot_mailbox_pa = desc.phys_addr;
		sev_snapshot_mailbox_len = desc.size;
		return;
	}
}
