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
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/linkage.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include <asm/current.h>
#include <asm/cacheflush.h>
#include <asm/cpu_entry_area.h>
#include <asm/desc.h>
#include <asm/e820/api.h>
#include <asm/fixmap.h>
#include <asm/fpu/api.h>
#include <asm/fpu/signal.h>
#include <asm/fpu/types.h>
#include <asm/fpu/xcr.h>
#include <asm/gsseg.h>
#include <asm/mem_encrypt.h>
#include <asm/msr.h>
#include <asm/paravirt.h>
#include <asm/pgtable_types.h>
#include <asm/pkru.h>
#include <asm/proto.h>
#include <asm/processor-flags.h>
#include <asm/sev.h>
#include <asm/segment.h>
#include <asm/smap.h>
#include <asm/special_insns.h>
#include <asm/setup.h>
#include <asm/trap_pf.h>
#include <asm/trapnr.h>
#include <asm/tlbflush.h>
#include <asm/unistd.h>
#include <asm/svm.h>

#include "sev_snapshot.h"

#define SEV_SNAPSHOT_MAILBOX_SIZE		SZ_2M
#define SEV_SNAPSHOT_FIXED_MAILBOX_PA		0x00600000ULL
#define SEV_SNAPSHOT_CONTROL_SHARED_PA		0x00200000ULL
#define SEV_SNAPSHOT_CONTROL_SHARED_SIZE	SZ_2M
#define SEV_SNAPSHOT_ATTESTATION_REQUEST_OFFSET \
	(SEV_SNAPSHOT_CONTROL_SHARED_SIZE - (2 * PAGE_SIZE))
#define SEV_SNAPSHOT_RESTORE_SHARED_BRIDGE_L2E_OFFSET	0x0E40ULL
#define SEV_SNAPSHOT_REQUEST_HOST_EVENT_SIGNAL	0x0051000000000014ULL

#define SEV_SNAPSHOT_SHARED_EXTENT_MAX \
	((SEV_SNAPSHOT_MAILBOX_SIZE - sizeof(struct sev_snapshot_mailbox)) / \
	 sizeof(struct sev_snapshot_shared_extent))

static u8 sev_snapshot_restore_xsave[PAGE_SIZE] __aligned(64);
static u8 sev_snapshot_restore_gdt_page[PAGE_SIZE] __page_aligned_bss;

static phys_addr_t sev_snapshot_mailbox_pa;
static u64 sev_snapshot_mailbox_len;
static raw_spinlock_t sev_snapshot_shared_lock;
static DEFINE_MUTEX(sev_snapshot_transport_lock);
static bool sev_snapshot_shared_ready;
static bool sev_snapshot_mailbox_accepted;
static struct task_struct *sev_snapshot_pending_task;
static void *sev_snapshot_attestation_request_page_va;
u64 *sev_snapshot_restore_bridge_l2e_va;
static bool sev_snapshot_warned_extent_capacity;
bool sev_snapshot_resume_in_progress;
static struct sev_snapshot_shared_extent
	sev_snapshot_shared_extents[SEV_SNAPSHOT_SHARED_EXTENT_MAX];
static u32 sev_snapshot_shared_extent_count;

static void sev_snapshot_map_request_page_locked(void);

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

static void sev_snapshot_install_sp0_scratch_stack(void)
{
#ifdef CONFIG_X86_64
	unsigned long scratch_top;

	if (!READ_ONCE(sev_snapshot_resume_in_progress))
		return;

	scratch_top = __this_cpu_ist_top_va(MCE);
	if (!scratch_top)
		return;

	load_sp0(scratch_top);
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

static void sev_snapshot_accept_mailbox(void)
{
	unsigned long vaddr;
	unsigned int npages;

	if (sev_snapshot_mailbox_accepted)
		return;

	if (!sev_snapshot_mailbox_pa || !sev_snapshot_mailbox_len)
		return;

	if (cc_platform_has(CC_ATTR_GUEST_SEV_SNP)) {
		vaddr = (unsigned long)__va(sev_snapshot_mailbox_pa);
		npages = sev_snapshot_mailbox_len >> PAGE_SHIFT;
		snp_set_memory_shared(vaddr, npages);
	}

	sev_snapshot_mailbox_accepted = true;
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

static void sev_snapshot_remove_shared_extent_locked(u64 gpa, u64 npages)
{
	u64 end = gpa + (npages << PAGE_SHIFT);
	u32 i;

	for (i = 0; i < sev_snapshot_shared_extent_count;) {
		struct sev_snapshot_shared_extent *extent =
			&sev_snapshot_shared_extents[i];
		u64 extent_start = extent->gpa;
		u64 extent_end = extent->gpa + (extent->npages << PAGE_SHIFT);

		if (end <= extent_start || gpa >= extent_end) {
			i++;
			continue;
		}

		if (gpa <= extent_start && end >= extent_end) {
			memmove(extent, extent + 1,
				(sev_snapshot_shared_extent_count - i - 1) *
				sizeof(*extent));
			sev_snapshot_shared_extent_count--;
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

		if (sev_snapshot_shared_extent_count >= SEV_SNAPSHOT_SHARED_EXTENT_MAX) {
			extent->npages = (gpa - extent_start) >> PAGE_SHIFT;
			i++;
			continue;
		}

		memmove(extent + 2, extent + 1,
			(sev_snapshot_shared_extent_count - i - 1) *
			sizeof(*extent));
		sev_snapshot_shared_extent_count++;
		extent[1].gpa = end;
		extent[1].npages = (extent_end - end) >> PAGE_SHIFT;
		extent->npages = (gpa - extent_start) >> PAGE_SHIFT;
		i += 2;
	}
}

static void sev_snapshot_add_shared_extent_locked(u64 gpa, u64 npages)
{
	u64 end = gpa + (npages << PAGE_SHIFT);
	u32 i;

	sev_snapshot_remove_shared_extent_locked(gpa, npages);

	for (i = 0; i < sev_snapshot_shared_extent_count; i++) {
		struct sev_snapshot_shared_extent *extent =
			&sev_snapshot_shared_extents[i];
		u64 extent_start = extent->gpa;
		u64 extent_end = extent->gpa + (extent->npages << PAGE_SHIFT);

		if (extent_end == gpa) {
			extent->npages += npages;
			return;
		}

		if (end == extent_start) {
			extent->gpa = gpa;
			extent->npages += npages;
			return;
		}
	}

	if (sev_snapshot_shared_extent_count >= SEV_SNAPSHOT_SHARED_EXTENT_MAX) {
		if (!sev_snapshot_warned_extent_capacity) {
			pr_warn("SEV snapshot shared extent table is full\n");
			sev_snapshot_warned_extent_capacity = true;
		}
		return;
	}

	sev_snapshot_shared_extents[sev_snapshot_shared_extent_count].gpa = gpa;
	sev_snapshot_shared_extents[sev_snapshot_shared_extent_count].npages =
		npages;
	sev_snapshot_shared_extent_count++;
}

void sev_snapshot_track_page_state_pa(unsigned long paddr, unsigned int npages,
				      enum psc_op op)
{
	unsigned long flags;
	u64 gpa;

	if (!npages)
		return;

	gpa = paddr & PAGE_MASK;

	raw_spin_lock_irqsave(&sev_snapshot_shared_lock, flags);
	if (op == SNP_PAGE_STATE_SHARED)
		sev_snapshot_add_shared_extent_locked(gpa, npages);
	else if (op == SNP_PAGE_STATE_PRIVATE)
		sev_snapshot_remove_shared_extent_locked(gpa, npages);
	raw_spin_unlock_irqrestore(&sev_snapshot_shared_lock, flags);
}

static void sev_snapshot_rebuild_shared_extents_from_direct_map(void)
{
	/* Shared/private transitions are tracked when they happen. */
}

static void sev_snapshot_flush_shared_extents(struct sev_snapshot_mailbox *mailbox)
{
	unsigned long flags;
	u32 count;

	if (!mailbox)
		return;

	raw_spin_lock_irqsave(&sev_snapshot_shared_lock, flags);
	count = min_t(u32, sev_snapshot_shared_extent_count,
		      SEV_SNAPSHOT_SHARED_EXTENT_MAX);
	memcpy(mailbox->shared_extents, sev_snapshot_shared_extents,
	       count * sizeof(*mailbox->shared_extents));
	WRITE_ONCE(mailbox->shared_extent_count, count);
	raw_spin_unlock_irqrestore(&sev_snapshot_shared_lock, flags);
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

	sev_snapshot_accept_mailbox();

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

	sev_snapshot_restore_bridge_l2e_va =
		(u64 *)((u8 *)sev_snapshot_attestation_request_page_va +
			SEV_SNAPSHOT_RESTORE_SHARED_BRIDGE_L2E_OFFSET);
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

			sev_snapshot_track_page_state((unsigned long)__va(range_start),
						      (next_gpa - range_start) >> PAGE_SHIFT,
						      op);
		range_start = gpa;
		next_gpa = gpa + PAGE_SIZE;
	}

	sev_snapshot_track_page_state((unsigned long)__va(range_start),
				      (next_gpa - range_start) >> PAGE_SHIFT,
				      op);
}

static void sev_snapshot_fill_xsave(struct sev_snapshot_cpu_context *ctx)
{
	struct fpstate *fpstate;
	u32 len;

	fpu_sync_fpstate(&current->thread.fpu);
	fpstate = current->thread.fpu.fpstate;
	if (!fpstate)
		return;

	ctx->xcr0 = xgetbv(XCR_XFEATURE_ENABLED_MASK);
	ctx->xstate_bv = fpstate->regs.xsave.header.xfeatures;
	ctx->xsave_max_size = PAGE_SIZE;
	ctx->valid_flags |= SEV_SNAPSHOT_CPU_CTX_F_XSAVE_META;

	len = min_t(u32, fpstate->size, PAGE_SIZE);
	if (!len)
		return;

	ctx->xsave_len = len;
	memcpy(ctx->xsave, &fpstate->regs.xsave, len);
	ctx->valid_flags |= SEV_SNAPSHOT_CPU_CTX_F_XSAVE_DATA;
}

static void sev_snapshot_fill_context(struct sev_snapshot_cpu_context *ctx,
				      const struct pt_regs *regs)
{
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
	memcpy(ctx->gdt_copy, get_cpu_gdt_rw(0),
	       min_t(size_t, sizeof(ctx->gdt_copy),
		     (size_t)ctx->gdt_desc.size + 1));
	store_idt(&ctx->idt);
	ctx->valid_flags |= SEV_SNAPSHOT_CPU_CTX_F_TABLES;

	ctx->pkru = read_pkru();
	ctx->valid_flags |= SEV_SNAPSHOT_CPU_CTX_F_PKRU;
	ctx->restore_gdt_page_va = (u64)sev_snapshot_restore_gdt_page;
	ctx->restore_gdt_page_pa = __pa_symbol(sev_snapshot_restore_gdt_page);

	sev_snapshot_fill_xsave(ctx);
}

static void __noreturn sev_snapshot_spin_after_capture(void)
{
	for (;;)
		asm volatile("pause" ::: "memory");
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
	mailbox = sev_snapshot_mailbox_va();
	if (!mailbox)
		return -ENODEV;

	return 0;
}

static long sev_snapshot_capture_final(const struct pt_regs *regs)
{
	struct sev_snapshot_mailbox *mailbox;
	long ret;

	if (!regs)
		return -EINVAL;

	mailbox = sev_snapshot_mailbox_va();
	if (!mailbox)
		return -ENODEV;

	memset(mailbox, 0, sizeof(*mailbox));
	sev_snapshot_fill_context(&mailbox->cpu_ctx, regs);
	sev_snapshot_rebuild_shared_extents_from_direct_map();
	sev_snapshot_flush_shared_extents(mailbox);
	clflush_cache_range(mailbox, SEV_SNAPSHOT_MAILBOX_SIZE);
	wmb();

	ret = sev_snapshot_signal_host_capture();
	if (ret)
		return ret;

	sev_snapshot_spin_after_capture();
}

long sev_snapshot_arm(void)
{
	long ret;

	ret = sev_snapshot_prepare_capture();
	if (ret) {
		WRITE_ONCE(sev_snapshot_pending_task, NULL);
		return ret;
	}

	WRITE_ONCE(sev_snapshot_pending_task, current);
	return 0;
}

void sev_snapshot_finalize_user_return(struct pt_regs *regs)
{
	struct task_struct *pending;
	long ret;

	if (!regs)
		return;

	pending = READ_ONCE(sev_snapshot_pending_task);
	if (pending != current)
		return;

	WRITE_ONCE(sev_snapshot_pending_task, NULL);

	if (regs->orig_ax != __NR_snapshot_enter)
		return;

	if (regs->ax < 0)
		return;

	ret = sev_snapshot_capture_final(regs);
	if (ret)
		regs->ax = ret;
}

static __always_inline void __noreturn sev_snapshot_restore_fault(void)
{
	BUG();
	unreachable();
}

static void sev_snapshot_restore_xsave_state(
	const struct sev_snapshot_cpu_context *ctx)
{
	if (!(ctx->valid_flags & SEV_SNAPSHOT_CPU_CTX_F_XSAVE_DATA) ||
	    !ctx->xsave_len)
		return;

	memset(sev_snapshot_restore_xsave, 0, sizeof(sev_snapshot_restore_xsave));
	memcpy(sev_snapshot_restore_xsave, ctx->xsave, ctx->xsave_len);

	xsetbv(XCR_XFEATURE_ENABLED_MASK, ctx->xcr0);

	asm volatile("xrstor %0"
		     : "=m" (sev_snapshot_restore_xsave)
		     : "m" (sev_snapshot_restore_xsave),
		       "a" ((u32)ctx->xstate_bv),
		       "d" ((u32)(ctx->xstate_bv >> 32))
		     : "memory");
}

static __always_inline struct x86_hw_tss *sev_snapshot_restore_cpu0_tss_va(void)
{
	return &((struct cpu_entry_area *)CPU_ENTRY_AREA_PER_CPU)->tss.x86_tss;
}

static void sev_snapshot_prepare_restore_gdt(
	const struct sev_snapshot_cpu_context *ctx,
	struct desc_ptr *gdt_desc)
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

	if (gdt_size > sizeof(ctx->gdt_copy) ||
	    gdt_size > sizeof(sev_snapshot_restore_gdt_page))
		sev_snapshot_restore_fault();

	gdt_base = sev_snapshot_restore_gdt_page;
	memcpy(gdt_base, ctx->gdt_copy, gdt_size);

	tss = (struct ldttss_desc *)((u8 *)gdt_base + tss_offset);

	/*
	 * Keep the final GDTR on a kernel-owned page that remains mapped after
	 * the restore bridge is no longer needed.  The mailbox copy is only the
	 * source for the snapshot GDT contents.
	 */
	live_tss = sev_snapshot_restore_cpu0_tss_va();
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

static void sev_snapshot_prefault_user_stack(const struct pt_regs *regs)
{
	u64 user_rsp;

	if (!regs)
		return;

	user_rsp = regs->sp;
	if (!user_rsp)
		return;

	stac();
	(void)READ_ONCE(*(u64 *)(unsigned long)user_rsp);
	clac();
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

void __noreturn sev_snapshot_restore_from_mailbox(
	struct sev_snapshot_mailbox *mailbox,
	u64 *bridge_l2e_record)
{
	struct sev_snapshot_cpu_context *ctx;
	struct desc_ptr restore_gdt_desc;
	struct pt_regs *regs;
	volatile u16 restore_ds, restore_es, restore_fs, restore_gs, restore_tss;

	if (!mailbox)
		sev_snapshot_restore_fault();

	(void)bridge_l2e_record;

	ctx = &mailbox->cpu_ctx;
	restore_ds = READ_ONCE(ctx->ds);
	restore_es = READ_ONCE(ctx->es);
	restore_fs = READ_ONCE(ctx->fs);
	restore_gs = READ_ONCE(ctx->gs);
	restore_tss = READ_ONCE(ctx->tss);

	sev_snapshot_prepare_restore_gdt(ctx, &restore_gdt_desc);
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

	regs = (struct pt_regs *)(unsigned long)ctx->kernel_regs_ptr;
	memcpy(regs, &ctx->regs, sizeof(*regs));
	regs->flags &= ~X86_EFLAGS_TF;

	write_cr0(ctx->cr0);
	write_cr2(ctx->cr2);
	wrmsrl(MSR_EFER, ctx->efer);

	sev_snapshot_register_runtime_ghcb();
	if (!sev_snapshot_negotiate_runtime_ghcb_protocol())
		sev_snapshot_restore_fault();

	wrmsrl(MSR_FS_BASE, ctx->fs_base);
	wrmsrl(MSR_GS_BASE, ctx->kernel_gs_base);
	wrmsrl(MSR_KERNEL_GS_BASE, ctx->user_gs_base);
	syscall_init();
	write_pkru(ctx->pkru);

	sev_snapshot_restore_xsave_state(ctx);

	sev_snapshot_prefault_user_stack(regs);
	sev_snapshot_install_sp0_scratch_stack();
	sev_snapshot_prefault_user_stack(regs);

	native_write_cr4(ctx->cr4);

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
		pr_info("snapshot mailbox descriptor: phys=0x%llx size=0x%llx\n",
			desc.phys_addr, desc.size);

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
