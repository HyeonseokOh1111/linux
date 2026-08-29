// SPDX-License-Identifier: GPL-2.0-only
/* AMD SEV-SNP guest-triggered snapshot support. */

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
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/tick.h>
#include <linux/uaccess.h>
#include <linux/virtio.h>
#include <linux/wait.h>

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>
#endif

#include <asm/current.h>
#include <asm/apic.h>
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
#include <asm/svm.h>

#include "sev_snapshot.h"

static_assert(ALIGN(sizeof(struct sev_snapshot_cpu_context) *
		    SEV_SNAPSHOT_MAX_CPUS +
		    sizeof(struct sev_snapshot_restore_barrier) +
		    sizeof(struct sev_snapshot_shared_extent_header), 64) +
	      SEV_SNAPSHOT_MAX_CPUS * PAGE_SIZE <=
	      SEV_SNAPSHOT_INPUT_PRIVATE_KEY_OFFSET);

#define SEV_SNAPSHOT_REQUEST_HOST_EVENT_SIGNAL	0x0051000000000014ULL

static u8 sev_snapshot_restore_gdt_pages[SEV_SNAPSHOT_MAX_CPUS][PAGE_SIZE]
	__page_aligned_bss;

static bool sev_snapshot_mailbox_present;
static cpumask_t sev_snapshot_saved_cpus_allowed;
static bool sev_snapshot_saved_cpus_allowed_valid;
static atomic_t sev_snapshot_v2_ap_ready = ATOMIC_INIT(0);
static atomic_t sev_snapshot_v2_ap_error = ATOMIC_INIT(0);
static atomic_t sev_snapshot_v2_ap_request_epoch = ATOMIC_INIT(0);
static atomic_t sev_snapshot_v2_capture_decision = ATOMIC_INIT(0);
static u32 sev_snapshot_v2_ap_cpu_count;
static void *sev_snapshot_v2_ap_mailbox;
static struct task_struct *sev_snapshot_v2_ap_tasks[SEV_SNAPSHOT_MAX_CPUS];
static DEFINE_MUTEX(sev_snapshot_v2_ap_task_lock);
static DECLARE_WAIT_QUEUE_HEAD(sev_snapshot_v2_ap_waitq);
/* Rollback note: disable these hooks together with Firecracker host replay re-enablement. */
static bool sev_snapshot_virtio_devices_frozen;

enum sev_snapshot_capture_decision {
	SEV_SNAPSHOT_CAPTURE_ABORT = -1,
	SEV_SNAPSHOT_CAPTURE_WAIT,
	SEV_SNAPSHOT_CAPTURE_COMMIT,
};
static __always_inline void __noreturn sev_snapshot_restore_fault(void);
static size_t sev_snapshot_cpu_bundle_len(u32 cpu_count);
static u32 *sev_snapshot_shared_extent_count_ptr(void *mailbox, u32 cpu_count);
static struct sev_snapshot_shared_extent *
sev_snapshot_shared_extents_ptr(void *mailbox, u32 cpu_count);

static __always_inline void *sev_snapshot_mailbox_va(void)
{
	if (!READ_ONCE(sev_snapshot_mailbox_present))
		return NULL;

	return __va(SEV_SNAPSHOT_FIXED_MAILBOX_PA);
}

bool sev_snapshot_mailbox_available(void)
{
	return READ_ONCE(sev_snapshot_mailbox_present);
}

static __always_inline void *sev_snapshot_restore_mailbox_va(void)
{
	return __va(SEV_SNAPSHOT_FIXED_MAILBOX_PA);
}

static __always_inline u8 *sev_snapshot_restore_gdt_page_for_cpu(unsigned int cpu)
{
	if (cpu >= SEV_SNAPSHOT_MAX_CPUS)
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

static int sev_snapshot_restore_current_affinity(void)
{
	cpumask_t allowed;
	int ret;

	if (!READ_ONCE(sev_snapshot_saved_cpus_allowed_valid))
		return 0;

	cpumask_and(&allowed, &sev_snapshot_saved_cpus_allowed,
		    cpu_online_mask);
	if (cpumask_empty(&allowed))
		cpumask_copy(&allowed, cpu_online_mask);

	ret = set_cpus_allowed_ptr(current, &allowed);
	WRITE_ONCE(sev_snapshot_saved_cpus_allowed_valid, false);
	return ret;
}

static void sev_snapshot_abort_capture_setup(void)
{
	if (READ_ONCE(sev_snapshot_virtio_devices_frozen)) {
		if (virtio_sev_snapshot_restore_devices())
			pr_warn("failed to restore virtio devices after snapshot abort\n");
		WRITE_ONCE(sev_snapshot_virtio_devices_frozen, false);
	}
	if (sev_snapshot_restore_current_affinity())
		pr_warn("failed to restore snapshot task CPU affinity\n");
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

static void sev_snapshot_restore_irq_stack_state(
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

static bool sev_snapshot_mapping_entry(unsigned long vaddr,
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

static bool sev_snapshot_kernel_image_page_is_shared(unsigned long paddr)
{
	unsigned long section_start = __pa_symbol(__start_bss_decrypted);
	unsigned long section_end = __pa_symbol(__start_bss_decrypted_unused);
	unsigned long vaddr, entry, size;
	unsigned int level;

	if (paddr < section_start || paddr >= section_end)
		return false;

	vaddr = (unsigned long)__start_bss_decrypted +
		(paddr - section_start);
	return sev_snapshot_mapping_entry(vaddr, &level, &entry, &size) &&
	       !(entry & sme_me_mask);
}

static int sev_snapshot_append_shared_extent(
	struct sev_snapshot_shared_extent *extents,
	u32 *count, u32 capacity, u64 gpa, u64 npages)
{
	struct sev_snapshot_shared_extent *last;

	if (!npages)
		return 0;

	if (*count) {
		last = &extents[*count - 1];
		if (last->gpa + (last->npages << PAGE_SHIFT) == gpa) {
			last->npages += npages;
			return 0;
		}
	}

	if (*count >= capacity)
		return -EOVERFLOW;

	extents[*count].gpa = gpa;
	extents[*count].npages = npages;
	(*count)++;
	return 0;
}

static int sev_snapshot_collect_shared_extents(void *mailbox, u32 cpu_count)
{
	struct sev_snapshot_shared_extent *extents;
	u32 *count_ptr;
	size_t extents_offset;
	unsigned long end_pfn = max_pfn_mapped;
	unsigned long pfn;
	u32 capacity;
	u32 count = 0;
	int ret;

	count_ptr = sev_snapshot_shared_extent_count_ptr(mailbox, cpu_count);
	extents = sev_snapshot_shared_extents_ptr(mailbox, cpu_count);
	extents_offset = (u8 *)extents - (u8 *)mailbox;
	if (extents_offset >= SEV_SNAPSHOT_MAILBOX_SIZE)
		return -EOVERFLOW;
	capacity = (SEV_SNAPSHOT_MAILBOX_SIZE - extents_offset) /
		   sizeof(*extents);
	WRITE_ONCE(*count_ptr, 0);

	if (!cc_platform_has(CC_ATTR_GUEST_SEV_SNP) || !sme_me_mask)
		return 0;

	for (pfn = 0; pfn < end_pfn;) {
		unsigned long paddr = PFN_PHYS(pfn);
		unsigned long vaddr = (unsigned long)__va(paddr);
		unsigned long entry, size, npages;
		unsigned int level;

		if (!sev_snapshot_mapping_entry(vaddr, &level, &entry, &size)) {
			if (sev_snapshot_kernel_image_page_is_shared(paddr)) {
				ret = sev_snapshot_append_shared_extent(
					extents, &count, capacity, paddr, 1);
				if (ret)
					return ret;
			}
			pfn++;
			continue;
		}

		npages = min_t(unsigned long, size >> PAGE_SHIFT,
				       end_pfn - pfn);
		if (!(entry & sme_me_mask)) {
			ret = sev_snapshot_append_shared_extent(extents, &count,
							 capacity, paddr, npages);
			if (ret)
				return ret;
		} else {
			unsigned long page_paddr;
			unsigned long scan_start = max(
				paddr,
				(unsigned long)__pa_symbol(__start_bss_decrypted));
			unsigned long scan_end = min(
				paddr + (npages << PAGE_SHIFT),
				(unsigned long)__pa_symbol(
					__start_bss_decrypted_unused));

			/*
			 * .bss..decrypted uses the kernel-image mapping's C-bit;
			 * its encrypted direct-map alias is not authoritative.
			 */
			for (page_paddr = scan_start; page_paddr < scan_end;
			     page_paddr += PAGE_SIZE) {
				if (!sev_snapshot_kernel_image_page_is_shared(
					    page_paddr))
					continue;
				ret = sev_snapshot_append_shared_extent(
					extents, &count, capacity, page_paddr, 1);
				if (ret)
					return ret;
			}
		}
		pfn += npages;
	}

	WRITE_ONCE(*count_ptr, count);
	return 0;
}

static bool sev_snapshot_landing_stack_rsp(unsigned long saved_sp,
					   unsigned long stack_bottom,
					   unsigned long stack_top,
					   unsigned long *landing_rsp)
{
	unsigned long anchor;
	unsigned long rsp;

	if (!landing_rsp || stack_bottom >= stack_top ||
	    stack_top - stack_bottom < SEV_SNAPSHOT_LANDING_STACK_SENTINEL_SIZE)
		return false;
	if (saved_sp < stack_bottom || saved_sp >= stack_top ||
	    saved_sp < SEV_SNAPSHOT_LANDING_STACK_SCRATCH_SIZE)
		return false;

	anchor = saved_sp - SEV_SNAPSHOT_LANDING_STACK_SCRATCH_SIZE;
	if (anchor < stack_bottom)
		return false;

	rsp = round_down(anchor, SEV_SNAPSHOT_LANDING_STACK_ALIGNMENT);
	if (rsp < SEV_SNAPSHOT_LANDING_STACK_SENTINEL_SIZE)
		return false;
	rsp -= SEV_SNAPSHOT_LANDING_STACK_SENTINEL_SIZE;
	if (rsp < stack_bottom ||
	    rsp > stack_top - SEV_SNAPSHOT_LANDING_STACK_SENTINEL_SIZE)
		return false;

	*landing_rsp = rsp;
	return true;
}

static int sev_snapshot_validate_kernel_continuation_stack(
	const struct sev_snapshot_kernel_continuation *continuation)
{
	unsigned long stack_page = (unsigned long)task_stack_page(current);
	unsigned long stack_top = stack_page + THREAD_SIZE;
	unsigned long stack_bottom = (unsigned long)end_of_stack(current);
	unsigned long landing_rsp;

	if (!continuation || stack_top < stack_page ||
	    stack_bottom + sizeof(unsigned long) < stack_bottom)
		return -ERANGE;
	/* Preserve the task-stack end magic below the usable stack range. */
	stack_bottom += sizeof(unsigned long);

	return sev_snapshot_landing_stack_rsp(continuation->sp, stack_bottom,
					      stack_top, &landing_rsp) ? 0 : -ERANGE;
}

#if IS_ENABLED(CONFIG_KUNIT)
static void sev_snapshot_shared_extent_coalesces(struct kunit *test)
{
	struct sev_snapshot_shared_extent extents[2] = {};
	u32 count = 0;

	KUNIT_EXPECT_EQ(test,
		sev_snapshot_append_shared_extent(extents, &count, 2,
						  0x1000, 1), 0);
	KUNIT_EXPECT_EQ(test,
		sev_snapshot_append_shared_extent(extents, &count, 2,
						  0x2000, 2), 0);
	KUNIT_EXPECT_EQ(test, count, 1U);
	KUNIT_EXPECT_EQ(test, extents[0].gpa, 0x1000ULL);
	KUNIT_EXPECT_EQ(test, extents[0].npages, 3ULL);
}

static void sev_snapshot_shared_extent_capacity_fails(struct kunit *test)
{
	struct sev_snapshot_shared_extent extent = {};
	u32 count = 0;

	KUNIT_ASSERT_EQ(test,
		sev_snapshot_append_shared_extent(&extent, &count, 1,
						  0x1000, 1), 0);
	KUNIT_EXPECT_EQ(test,
		sev_snapshot_append_shared_extent(&extent, &count, 1,
						  0x3000, 1), -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, count, 1U);
}

static void sev_snapshot_landing_stack_accepts_valid_task_stack(struct kunit *test)
{
	unsigned long landing_rsp = 0;

	KUNIT_EXPECT_TRUE(test,
		sev_snapshot_landing_stack_rsp(0x4008, 0x1000, 0x5000,
					       &landing_rsp));
	KUNIT_EXPECT_EQ(test, landing_rsp, 0x37f8UL);
}

static void sev_snapshot_landing_stack_rejects_invalid_ranges(struct kunit *test)
{
	unsigned long landing_rsp = 0;

	KUNIT_EXPECT_FALSE(test,
		sev_snapshot_landing_stack_rsp(0x700, 0, 0x1000,
					       &landing_rsp));
	KUNIT_EXPECT_FALSE(test,
		sev_snapshot_landing_stack_rsp(0x5000, 0x1000, 0x5000,
					       &landing_rsp));
	KUNIT_EXPECT_FALSE(test,
		sev_snapshot_landing_stack_rsp(0x3700, 0x3000, 0x5000,
					       &landing_rsp));
	KUNIT_EXPECT_FALSE(test,
		sev_snapshot_landing_stack_rsp(0x3808, 0x3000, 0x5000,
					       &landing_rsp));
}

static struct kunit_case sev_snapshot_shared_extent_cases[] = {
	KUNIT_CASE(sev_snapshot_shared_extent_coalesces),
	KUNIT_CASE(sev_snapshot_shared_extent_capacity_fails),
	KUNIT_CASE(sev_snapshot_landing_stack_accepts_valid_task_stack),
	KUNIT_CASE(sev_snapshot_landing_stack_rejects_invalid_ranges),
	{}
};

static struct kunit_suite sev_snapshot_shared_extent_suite = {
	.name = "sev_snapshot_shared_extent",
	.test_cases = sev_snapshot_shared_extent_cases,
};

kunit_test_suite(sev_snapshot_shared_extent_suite);
#endif

static int sev_snapshot_fill_xsave(struct sev_snapshot_cpu_context *ctx)
{
	struct fpu *fpu;
	struct fpstate *fpstate;
	int ret = 0;

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
	if (!fpstate) {
		ret = -ENODATA;
		goto out_unlock;
	}

	if (fpstate->size > sizeof(ctx->xsave)) {
		ret = -EOVERFLOW;
		goto out_unlock;
	}

	memcpy(ctx->xsave, &fpstate->regs.xsave, fpstate->size);
out_unlock:
	fpregs_unlock();
	return ret;
}

static int sev_snapshot_fill_context(struct sev_snapshot_cpu_context *ctx)
{
	unsigned int cpu = smp_processor_id();
	int ret;

	if (!ctx || cpu >= SEV_SNAPSHOT_MAX_CPUS)
		return -EINVAL;
	ret = sev_snapshot_validate_kernel_continuation_stack(&ctx->continuation);
	if (ret)
		return ret;

	ctx->restore_mailbox_va = (u64)sev_snapshot_restore_mailbox_va();

	rdmsrl(MSR_FS_BASE, ctx->fs_base);
	rdmsrl(MSR_GS_BASE, ctx->kernel_gs_base);
	rdmsrl(MSR_KERNEL_GS_BASE, ctx->user_gs_base);

	ctx->cr0 = read_cr0();
	ctx->cr3 = __read_cr3();
	ctx->cr4 = __read_cr4();

	rdmsrl(MSR_EFER, ctx->efer);

	memcpy(ctx->gdt_copy, get_cpu_gdt_rw(cpu), sizeof(ctx->gdt_copy));

	ctx->pkru = read_pkru();
	ctx->xcr0 = xgetbv(XCR_XFEATURE_ENABLED_MASK);
	ctx->restore_gdt_page_va =
		(u64)sev_snapshot_restore_gdt_page_for_cpu(cpu);
	ctx->restore_gdt_page_pa =
		__pa_symbol(sev_snapshot_restore_gdt_pages) +
		(u64)cpu * PAGE_SIZE;
	ctx->restore_from_mailbox_rip = (u64)sev_snapshot_restore_from_mailbox;
#ifdef CONFIG_X86_64
	ctx->hardirq_stack_ptr = (u64)this_cpu_read(pcpu_hot.hardirq_stack_ptr);
#endif

	if (!ctx->continuation.ip || !ctx->continuation.sp ||
	    !ctx->restore_mailbox_va || !ctx->restore_gdt_page_va ||
	    !ctx->restore_gdt_page_pa || !ctx->restore_from_mailbox_rip ||
	    !ctx->hardirq_stack_ptr)
		return -ENODATA;

	ret = sev_snapshot_fill_xsave(ctx);
	if (ret)
		return ret;

	return 0;
}

static void __noreturn sev_snapshot_spin_after_capture(void)
{
	for (;;)
		asm volatile("pause" ::: "memory");
}

static void sev_snapshot_signal_host_capture(void)
{
	u64 ghcb_msr;

	ghcb_msr = __rdmsr(MSR_AMD64_SEV_ES_GHCB);
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB,
		      SEV_SNAPSHOT_REQUEST_HOST_EVENT_SIGNAL);
	VMGEXIT();
	native_wrmsrl(MSR_AMD64_SEV_ES_GHCB, ghcb_msr);
}

static int sev_snapshot_prepare_capture(void)
{
	if (!sev_snapshot_mailbox_va())
		return -ENODEV;

	return 0;
}

static bool sev_snapshot_cpu_count_valid(u32 cpu_count)
{
	return cpu_count >= SEV_SNAPSHOT_MIN_CPUS &&
	       cpu_count <= SEV_SNAPSHOT_MAX_CPUS;
}

static bool sev_snapshot_online_cpus_match(u32 cpu_count)
{
	u32 cpu;

	if (!sev_snapshot_cpu_count_valid(cpu_count))
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
	return num_online_cpus();
}

static size_t sev_snapshot_cpu_context_offset(unsigned int cpu)
{
	return sizeof(struct sev_snapshot_cpu_context) * cpu;
}

static size_t sev_snapshot_restore_barrier_offset(u32 cpu_count)
{
	return sizeof(struct sev_snapshot_cpu_context) * cpu_count;
}

static size_t sev_snapshot_cpu_bundle_len(u32 cpu_count)
{
	size_t len = sev_snapshot_restore_barrier_offset(cpu_count);

	if (cpu_count > SEV_SNAPSHOT_MIN_CPUS)
		len += sizeof(struct sev_snapshot_restore_barrier);

	return len;
}

static struct sev_snapshot_cpu_context *
sev_snapshot_cpu_context(void *mailbox, u32 cpu_count, unsigned int cpu)
{
	if (!mailbox || !sev_snapshot_cpu_count_valid(cpu_count) || cpu >= cpu_count)
		sev_snapshot_restore_fault();

	return (struct sev_snapshot_cpu_context *)
		((u8 *)mailbox + sev_snapshot_cpu_context_offset(cpu));
}

static struct sev_snapshot_restore_barrier *
sev_snapshot_restore_barrier(void *mailbox, u32 cpu_count)
{
	if (!mailbox || cpu_count <= SEV_SNAPSHOT_MIN_CPUS ||
	    cpu_count > SEV_SNAPSHOT_MAX_CPUS)
		sev_snapshot_restore_fault();

	return (struct sev_snapshot_restore_barrier *)
		((u8 *)mailbox + sev_snapshot_restore_barrier_offset(cpu_count));
}

static u32 *sev_snapshot_shared_extent_count_ptr(void *mailbox, u32 cpu_count)
{
	struct sev_snapshot_shared_extent_header *header;

	header = (struct sev_snapshot_shared_extent_header *)
		((u8 *)mailbox + sev_snapshot_cpu_bundle_len(cpu_count));
	return &header->shared_extent_count;
}

static struct sev_snapshot_shared_extent *
sev_snapshot_shared_extents_ptr(void *mailbox, u32 cpu_count)
{
	return (struct sev_snapshot_shared_extent *)
		((u8 *)sev_snapshot_shared_extent_count_ptr(mailbox, cpu_count) +
		 sizeof(struct sev_snapshot_shared_extent_header));
}

static void sev_snapshot_init_cpu_bundle(void *mailbox, u32 cpu_count)
{
	size_t bundle_len = sev_snapshot_cpu_bundle_len(cpu_count);

	memset(mailbox, 0,
	       bundle_len + sizeof(struct sev_snapshot_shared_extent_header));
}

static __always_inline void
sev_snapshot_store_current_kernel_continuation(
	struct sev_snapshot_kernel_continuation *continuation,
	unsigned long ip)
{
	memset(continuation, 0, sizeof(*continuation));

	asm volatile("movq %%r15, %c[r15](%[continuation])\n\t"
		     "movq %%r14, %c[r14](%[continuation])\n\t"
		     "movq %%r13, %c[r13](%[continuation])\n\t"
		     "movq %%r12, %c[r12](%[continuation])\n\t"
		     "movq %%rbp, %c[bp](%[continuation])\n\t"
		     "movq %%rbx, %c[bx](%[continuation])\n\t"
		     "movq %%r11, %c[r11](%[continuation])\n\t"
		     "movq %%r10, %c[r10](%[continuation])\n\t"
		     "movq %%r9, %c[r9](%[continuation])\n\t"
		     "movq %%r8, %c[r8](%[continuation])\n\t"
		     "movq %%rax, %c[ax](%[continuation])\n\t"
		     "movq %%rcx, %c[cx](%[continuation])\n\t"
		     "movq %%rdx, %c[dx](%[continuation])\n\t"
		     "movq %%rsi, %c[si](%[continuation])\n\t"
		     "movq %%rdi, %c[di](%[continuation])\n\t"
		     "movq %%rsp, %c[sp](%[continuation])\n\t"
		     "pushfq\n\t"
		     "popq %%rax\n\t"
		     "movq %%rax, %c[flags](%[continuation])\n\t"
		     :
		     : [continuation] "r" (continuation),
		       [r15] "i" (offsetof(struct sev_snapshot_kernel_continuation, r15)),
		       [r14] "i" (offsetof(struct sev_snapshot_kernel_continuation, r14)),
		       [r13] "i" (offsetof(struct sev_snapshot_kernel_continuation, r13)),
		       [r12] "i" (offsetof(struct sev_snapshot_kernel_continuation, r12)),
		       [bp] "i" (offsetof(struct sev_snapshot_kernel_continuation, bp)),
		       [bx] "i" (offsetof(struct sev_snapshot_kernel_continuation, bx)),
		       [r11] "i" (offsetof(struct sev_snapshot_kernel_continuation, r11)),
		       [r10] "i" (offsetof(struct sev_snapshot_kernel_continuation, r10)),
		       [r9] "i" (offsetof(struct sev_snapshot_kernel_continuation, r9)),
		       [r8] "i" (offsetof(struct sev_snapshot_kernel_continuation, r8)),
		       [ax] "i" (offsetof(struct sev_snapshot_kernel_continuation, ax)),
		       [cx] "i" (offsetof(struct sev_snapshot_kernel_continuation, cx)),
		       [dx] "i" (offsetof(struct sev_snapshot_kernel_continuation, dx)),
		       [si] "i" (offsetof(struct sev_snapshot_kernel_continuation, si)),
		       [di] "i" (offsetof(struct sev_snapshot_kernel_continuation, di)),
		       [sp] "i" (offsetof(struct sev_snapshot_kernel_continuation, sp)),
		       [flags] "i" (offsetof(struct sev_snapshot_kernel_continuation, flags))
		     : "rax", "memory");

	continuation->ip = ip;
	continuation->flags &= ~X86_EFLAGS_TF;
}

static void sev_snapshot_capture_secondary_cpu_v2(void)
{
	void *mailbox = READ_ONCE(sev_snapshot_v2_ap_mailbox);
	struct sev_snapshot_cpu_context *ctx;
	u32 cpu_count = READ_ONCE(sev_snapshot_v2_ap_cpu_count);
	unsigned int cpu = raw_smp_processor_id();
	unsigned long flags;
	int decision;
	int ret;

	if (!mailbox || !sev_snapshot_cpu_count_valid(cpu_count) ||
	    cpu == 0 || cpu >= cpu_count) {
		pr_warn("multi-vCPU AP rendezvous capture failed: cpu=%u cpu_count=%u mailbox=%px\n",
			cpu, cpu_count, mailbox);
		atomic_set(&sev_snapshot_v2_ap_error, 1);
		atomic_inc(&sev_snapshot_v2_ap_ready);
		return;
	}

	ctx = sev_snapshot_cpu_context(mailbox, cpu_count, cpu);
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
	sev_snapshot_store_current_kernel_continuation(
		&ctx->continuation, (unsigned long)&&restore_continue);

	/*
	 * APs checkpoint from cooperative kernel threads. Capture the
	 * hardware XSAVE image as part of the guest-owned context so the runtime
	 * can validate and replay the AP restore state instead of synthesizing it.
	 */
	ret = sev_snapshot_fill_context(ctx);
	if (ret) {
		atomic_set(&sev_snapshot_v2_ap_error, 1);
		atomic_inc(&sev_snapshot_v2_ap_ready);
		local_irq_restore(flags);
		preempt_enable_no_resched();
		return;
	}
	tick_suspend_local();
	smp_wmb();
	atomic_inc(&sev_snapshot_v2_ap_ready);
	do {
		decision = atomic_read_acquire(
			&sev_snapshot_v2_capture_decision);
		cpu_relax();
	} while (decision == SEV_SNAPSHOT_CAPTURE_WAIT);

	if (decision == SEV_SNAPSHOT_CAPTURE_ABORT) {
		tick_resume_local();
		local_irq_restore(flags);
		preempt_enable_no_resched();
		return;
	}

	/*
	 * The capture path below never returns, but restore enters this function
	 * at restore_continue through the saved continuation.  Keep that label a
	 * real compiler control-flow target so it cannot be folded into the last
	 * reachable capture-path instruction.
	 */
	asm goto("" ::: "memory" : restore_continue);
	sev_snapshot_signal_host_capture();
	sev_snapshot_spin_after_capture();

restore_continue:
	local_irq_restore(flags);
	preempt_enable_no_resched();
}

static int sev_snapshot_v2_ap_thread_fn(void *unused)
{
	int seen_epoch = 0;

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

	if (cpu == 0 || cpu >= SEV_SNAPSHOT_MAX_CPUS || !cpu_online(cpu))
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

	if (!sev_snapshot_online_cpus_match(cpu_count) ||
	    cpu_count <= SEV_SNAPSHOT_MIN_CPUS)
		return -EINVAL;

	for (cpu = 1; cpu < cpu_count; cpu++) {
		ret = sev_snapshot_v2_ensure_ap_thread(cpu);
		if (ret)
			return ret;
	}

	return 0;
}

static long sev_snapshot_rendezvous_secondary_cpu_v2(void *mailbox,
						     u32 cpu_count)
{
	int expected_ready = (int)cpu_count - 1;
	u32 cpu;
	unsigned int wait;

	if (!mailbox || !sev_snapshot_online_cpus_match(cpu_count) ||
	    cpu_count <= SEV_SNAPSHOT_MIN_CPUS)
		return -EINVAL;

	for (cpu = 1; cpu < cpu_count; cpu++) {
		if (!READ_ONCE(sev_snapshot_v2_ap_tasks[cpu]))
			return -ENODEV;
	}

	atomic_set(&sev_snapshot_v2_ap_ready, 0);
	atomic_set(&sev_snapshot_v2_ap_error, 0);
	atomic_set(&sev_snapshot_v2_capture_decision,
		   SEV_SNAPSHOT_CAPTURE_WAIT);
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
		atomic_set_release(&sev_snapshot_v2_capture_decision,
				   SEV_SNAPSHOT_CAPTURE_ABORT);
		WRITE_ONCE(sev_snapshot_v2_ap_mailbox, NULL);
		WRITE_ONCE(sev_snapshot_v2_ap_cpu_count, 0);
		return -ETIMEDOUT;
	}
	if (atomic_read(&sev_snapshot_v2_ap_error)) {
		atomic_set_release(&sev_snapshot_v2_capture_decision,
				   SEV_SNAPSHOT_CAPTURE_ABORT);
		WRITE_ONCE(sev_snapshot_v2_ap_mailbox, NULL);
		WRITE_ONCE(sev_snapshot_v2_ap_cpu_count, 0);
		return -EIO;
	}

	return 0;
}

long sev_snapshot_arm(void)
{
	struct sev_snapshot_cpu_context *ctx;
	unsigned long flags;
	void *mailbox;
	u32 cpu_count;
	bool ap_quiesced = false;
	bool tick_suspended = false;
	long ret;

	ret = sev_snapshot_prepare_shadow_crypto();
	if (ret)
		return ret;

	ret = sev_snapshot_pin_current_to_boot_cpu();
	if (ret) {
		sev_snapshot_abort_capture_setup();
		return ret;
	}

	cpu_count = sev_snapshot_restore_cpu_count();
	if (!sev_snapshot_online_cpus_match(cpu_count)) {
		sev_snapshot_abort_capture_setup();
		return -EINVAL;
	}
	if (cpu_count > SEV_SNAPSHOT_MIN_CPUS) {
		ret = sev_snapshot_v2_ensure_ap_threads(cpu_count);
		if (ret) {
			sev_snapshot_abort_capture_setup();
			return ret;
		}
	}

	ret = sev_snapshot_prepare_capture();
	if (ret)
		goto out_abort;
	mailbox = sev_snapshot_mailbox_va();
	if (!mailbox) {
		ret = -ENODEV;
		goto out_abort;
	}

	sev_snapshot_init_cpu_bundle(mailbox, cpu_count);
	ret = sev_snapshot_freeze_virtio_devices();
	if (ret)
		goto out_abort;
	if (cpu_count > SEV_SNAPSHOT_MIN_CPUS) {
		ret = sev_snapshot_rendezvous_secondary_cpu_v2(mailbox,
							       cpu_count);
		if (ret)
			goto out_abort;
		ap_quiesced = true;
	}

	ctx = sev_snapshot_cpu_context(mailbox, cpu_count, 0);
	preempt_disable();
	local_irq_save(flags);
	sev_snapshot_store_current_kernel_continuation(
		&ctx->continuation, (unsigned long)&&restore_continue);
	ret = sev_snapshot_fill_context(ctx);
	if (ret)
		goto out_enable;

	tick_suspend_local();
	tick_suspended = true;
	ret = sev_snapshot_collect_shared_extents(mailbox, cpu_count);
	if (ret)
		goto out_enable;
	clflush_cache_range(mailbox, SEV_SNAPSHOT_MAILBOX_SIZE);
	wmb();
	if (ap_quiesced) {
		atomic_set_release(&sev_snapshot_v2_capture_decision,
				   SEV_SNAPSHOT_CAPTURE_COMMIT);
		ap_quiesced = false;
	}
	/* See the AP capture path above for why this empty edge is required. */
	asm goto("" ::: "memory" : restore_continue);
	sev_snapshot_signal_host_capture();
	sev_snapshot_spin_after_capture();

restore_continue:
	local_irq_restore(flags);
	preempt_enable_no_resched();
	return sev_snapshot_restore_current_affinity();

out_enable:
	if (tick_suspended)
		tick_resume_local();
	if (ap_quiesced)
		atomic_set_release(&sev_snapshot_v2_capture_decision,
				   SEV_SNAPSHOT_CAPTURE_ABORT);
	local_irq_restore(flags);
	preempt_enable_no_resched();
out_abort:
	sev_snapshot_abort_capture_setup();
	return ret;
}

static __always_inline void __noreturn sev_snapshot_restore_fault(void)
{
	BUG();
	unreachable();
}

static void *sev_snapshot_restore_xsave_scratch(void *mailbox,
						u64 restore_cpu_id,
						u32 cpu_count)
{
	unsigned long offset;

	offset = ALIGN(sev_snapshot_cpu_bundle_len(cpu_count) +
		       sizeof(struct sev_snapshot_shared_extent_header),
		       64);
	offset += restore_cpu_id * PAGE_SIZE;
	if (offset + PAGE_SIZE > SEV_SNAPSHOT_MAILBOX_SIZE)
		sev_snapshot_restore_fault();

	return (u8 *)mailbox + offset;
}

static void sev_snapshot_restore_xsave_state(
	void *mailbox,
	const struct sev_snapshot_cpu_context *ctx,
	u64 restore_cpu_id,
	u32 cpu_count)
{
	u64 xfeatures;
	void *scratch;

	xsetbv(XCR_XFEATURE_ENABLED_MASK, ctx->xcr0);
	scratch = sev_snapshot_restore_xsave_scratch(mailbox, restore_cpu_id,
						       cpu_count);
	memcpy(scratch, ctx->xsave, PAGE_SIZE);
	xfeatures = ((struct xregs_state *)scratch)->header.xfeatures;

	asm volatile("xrstor (%[scratch])"
			 :
			 : [scratch] "r" (scratch),
			       "a" ((u32)xfeatures),
			       "d" ((u32)(xfeatures >> 32))
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
	const size_t tss_offset = GDT_ENTRY_TSS * 8;
	void *gdt_base;

	if (tss_offset + sizeof(*tss) > GDT_SIZE)
		sev_snapshot_restore_fault();

	gdt_base = sev_snapshot_restore_gdt_page_for_cpu(restore_cpu_id);
	memcpy(gdt_base, ctx->gdt_copy, GDT_SIZE);

	tss = (struct ldttss_desc *)((u8 *)gdt_base + tss_offset);

	/*
	 * Keep the final GDTR on a kernel-owned page that remains mapped after
	 * the restore bridge is no longer needed.  The mailbox copy is only the
	 * source for the snapshot GDT contents.
	 */
	live_tss = sev_snapshot_restore_tss_va(restore_cpu_id);
	set_tssldt_descriptor(tss, (unsigned long)live_tss, DESC_TSS,
			      __KERNEL_TSS_LIMIT);

	gdt_desc->size = GDT_SIZE - 1;
	gdt_desc->address = (unsigned long)gdt_base;
}

static void sev_snapshot_restore_tr_from_gdt(void)
{
	u16 restore_tss = GDT_ENTRY_TSS * 8;

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

static struct sev_snapshot_cpu_context *sev_snapshot_select_restore_context(
	void *mailbox,
	u64 restore_cpu_id)
{
	u32 cpu_count = sev_snapshot_restore_cpu_count();

	if (!mailbox || !sev_snapshot_cpu_count_valid(cpu_count))
		sev_snapshot_restore_fault();
	if (restore_cpu_id >= cpu_count)
		sev_snapshot_restore_fault();

	return sev_snapshot_cpu_context(mailbox, cpu_count, restore_cpu_id);
}

static void sev_snapshot_restore_arch_context(
	const struct sev_snapshot_cpu_context *ctx,
	u64 restore_cpu_id)
{
	struct desc_ptr restore_gdt_desc;

	sev_snapshot_prepare_restore_gdt(ctx, &restore_gdt_desc, restore_cpu_id);
	load_gdt(&restore_gdt_desc);
	sev_snapshot_reload_kernel_cs();
	asm volatile("movw %[ds], %%ss\n\t"
		     "movw %[ds], %%ds\n\t"
		     "movw %[ds], %%es\n\t"
		     :
			     : [ds] "r" ((__u16)__KERNEL_DS)
			     : "memory");

	sev_snapshot_restore_tr_from_gdt();

	write_cr0(ctx->cr0);
	wrmsrl(MSR_EFER, ctx->efer);

	wrmsrl(MSR_FS_BASE, ctx->fs_base);
	wrmsrl(MSR_GS_BASE, ctx->kernel_gs_base);
	wrmsrl(MSR_KERNEL_GS_BASE, ctx->user_gs_base);
	setup_ghcb_for_snapshot_restore();
	/*
	 * Activate the restored TSS, runtime GHCB, and runtime #VC IDT before
	 * writing any intercepted MSR.  The restore continues in scratch space
	 * below the saved kernel continuation stack pointer; normal interrupt entry
	 * uses the restored hardirq stack after its pointer and in-use state are
	 * restored below.
	 */
	load_current_idt();
}

static void sev_snapshot_finish_arch_context_on_landing_stack(
	void *mailbox,
	const struct sev_snapshot_cpu_context *ctx,
	u64 restore_cpu_id,
	u32 restore_cpu_count)
{
	/*
	 * Intercepted syscall and TSC_AUX MSRs are written only after the
	 * runtime GHCB, TSS, and runtime #VC IDT are active.  Restore the normal
	 * hardirq-stack state before device restore enables interrupts; the saved
	 * kernel task stack remains the current C stack.
	 */
	sev_snapshot_restore_irq_stack_state(ctx, restore_cpu_id);
	syscall_init();
	setup_getcpu_msr(restore_cpu_id);
	write_pkru(ctx->pkru);

	sev_snapshot_restore_xsave_state(mailbox, ctx, restore_cpu_id,
					restore_cpu_count);
}

static void sev_snapshot_restore_cpu_runtime(u64 restore_cpu_id,
					     u32 restore_cpu_count)
{
	/*
	 * Template APs start with fresh local-APIC state.  Reuse the normal
	 * secondary-CPU setup before restoring their KVM clock and local tick so
	 * cross-CPU IPIs are available to resumed kernel code.  A multi-vCPU BSP
	 * waits until every AP is interruptible before enabling its APIC below.
	 */
	if (restore_cpu_id != 0) {
		apic_ap_setup();
		kvm_sev_snapshot_restore_reinit_ap();
		tick_resume_local();
		return;
	}

	kvm_sev_snapshot_restore_reinit();
	if (restore_cpu_count == SEV_SNAPSHOT_MIN_CPUS) {
		/*
		 * The Template BSP also starts with fresh local-APIC hardware
		 * state.  Install it before resuming the restored tick, just as
		 * the multi-vCPU BSP does after its AP rendezvous below.
		 */
		apic_ap_setup();
		tick_resume_local();
	}
}

static void sev_snapshot_restore_rendezvous(void *mailbox,
					    u32 restore_cpu_count,
					    u64 restore_cpu_id)
{
	struct sev_snapshot_restore_barrier *restore_barrier = NULL;

	if (restore_cpu_count > SEV_SNAPSHOT_MIN_CPUS) {
		restore_barrier = sev_snapshot_restore_barrier(mailbox,
							 restore_cpu_count);
		atomic_inc_return_release((atomic_t *)&restore_barrier->arrived);
		if (restore_cpu_id == 0) {
			while (atomic_read_acquire(
				       (atomic_t *)&restore_barrier->arrived) <
			       restore_cpu_count)
				cpu_relax();
		} else {
			/*
			 * Device restore can issue synchronous TLB-shootdown IPIs.
			 * Keep the restored AP interruptible while the BSP performs
			 * global restore, then return to the saved continuation with
			 * interrupts disabled as required by the handoff path.
			 */
			local_irq_enable();
			while (!atomic_read_acquire(
					(atomic_t *)&restore_barrier->resume))
				cpu_relax();
			local_irq_disable();
			return;
		}
	}

	if (restore_cpu_id != 0)
		return;

	if (restore_barrier) {
		/*
		 * Cross-CPU callbacks used by device restore require a live BSP
		 * local APIC.  Defer this BSP-only setup until every AP has installed
		 * its own APIC and is waiting with interrupts enabled.
		 */
		apic_ap_setup();
		tick_resume_local();
	}

	/* Userspace remains stopped until every frozen device is restored. */
	if (sev_snapshot_restore_virtio_devices())
		sev_snapshot_restore_fault();

	if (restore_barrier)
		atomic_set_release((atomic_t *)&restore_barrier->resume, 1);
}

static void __noreturn sev_snapshot_resume_saved_continuation(
	const struct sev_snapshot_cpu_context *ctx,
	u64 restore_cpu_id)
{
	local_irq_disable();
	sev_snapshot_restore_irq_stack_state(ctx, restore_cpu_id);
	sev_snapshot_resume_kernel_continuation(&ctx->continuation);
	unreachable();
}

void __noreturn sev_snapshot_restore_from_mailbox(
	void *mailbox,
	u64 restore_cpu_id)
{
	struct sev_snapshot_cpu_context *ctx;
	u32 restore_cpu_count = sev_snapshot_restore_cpu_count();

	ctx = sev_snapshot_select_restore_context(mailbox, restore_cpu_id);
	sev_snapshot_restore_arch_context(ctx, restore_cpu_id);
	sev_snapshot_finish_arch_context_on_landing_stack(
		mailbox, ctx, restore_cpu_id, restore_cpu_count);
	sev_snapshot_restore_cpu_runtime(restore_cpu_id, restore_cpu_count);
	sev_snapshot_restore_rendezvous(mailbox, restore_cpu_count,
					restore_cpu_id);
	sev_snapshot_resume_saved_continuation(ctx, restore_cpu_id);
}

void __init sev_mailbox_reserve_from_setup_data(struct boot_params *bp)
{
	u64 pa_data;

	if (!bp)
		return;

	pa_data = bp->hdr.setup_data;
	while (pa_data) {
		struct setup_data *hdr;
		u64 next;

		hdr = early_memremap(pa_data, sizeof(*hdr));
		if (!hdr) {
			pr_err("failed to map setup_data while scanning for SEV snapshot mailbox\n");
			return;
		}

		next = hdr->next;

		if (hdr->type != SETUP_SEV_SNAPSHOT_MAILBOX) {
			early_memunmap(hdr, sizeof(*hdr));
			pa_data = next;
			continue;
		}

		if (hdr->len != 0) {
			early_memunmap(hdr, sizeof(*hdr));
			pr_err("ignoring SEV snapshot mailbox marker with payload\n");
			return;
		}

		early_memunmap(hdr, sizeof(*hdr));

		if (!sev_snapshot_range_is_ram(SEV_SNAPSHOT_FIXED_MAILBOX_PA,
					       SEV_SNAPSHOT_MAILBOX_SIZE))
			pr_warn("SEV snapshot mailbox backing is not visible yet during early setup; reserving fixed range anyway\n");

		if (memblock_is_region_reserved(SEV_SNAPSHOT_FIXED_MAILBOX_PA,
						SEV_SNAPSHOT_MAILBOX_SIZE)) {
			pr_err("SEV snapshot mailbox overlaps an already reserved range\n");
			return;
		}

		if (memblock_reserve(SEV_SNAPSHOT_FIXED_MAILBOX_PA,
				     SEV_SNAPSHOT_MAILBOX_SIZE)) {
			pr_err("failed to reserve SEV snapshot mailbox range\n");
			return;
		}

		WRITE_ONCE(sev_snapshot_mailbox_present, true);
		return;
	}
}
