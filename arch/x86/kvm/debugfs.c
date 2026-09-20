// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel-based Virtual Machine driver for Linux
 *
 * Copyright 2016 Red Hat, Inc. and/or its affiliates.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kvm_host.h>
#include <linux/debugfs.h>
#include <linux/hugetlb.h>
#include <linux/xarray.h>
#include "severifast_profile.h"
#include "lapic.h"
#include "mmu.h"
#include "mmu/mmu_internal.h"

static int vcpu_get_timer_advance_ns(void *data, u64 *val)
{
	struct kvm_vcpu *vcpu = (struct kvm_vcpu *) data;
	*val = vcpu->arch.apic->lapic_timer.timer_advance_ns;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(vcpu_timer_advance_ns_fops, vcpu_get_timer_advance_ns, NULL, "%llu\n");

static int vcpu_get_guest_mode(void *data, u64 *val)
{
	struct kvm_vcpu *vcpu = (struct kvm_vcpu *) data;
	*val = vcpu->stat.guest_mode;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(vcpu_guest_mode_fops, vcpu_get_guest_mode, NULL, "%lld\n");

static int vcpu_get_tsc_offset(void *data, u64 *val)
{
	struct kvm_vcpu *vcpu = (struct kvm_vcpu *) data;
	*val = vcpu->arch.tsc_offset;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(vcpu_tsc_offset_fops, vcpu_get_tsc_offset, NULL, "%lld\n");

static int vcpu_get_tsc_scaling_ratio(void *data, u64 *val)
{
	struct kvm_vcpu *vcpu = (struct kvm_vcpu *) data;
	*val = vcpu->arch.tsc_scaling_ratio;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(vcpu_tsc_scaling_fops, vcpu_get_tsc_scaling_ratio, NULL, "%llu\n");

static int vcpu_get_tsc_scaling_frac_bits(void *data, u64 *val)
{
	*val = kvm_caps.tsc_scaling_ratio_frac_bits;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(vcpu_tsc_scaling_frac_fops, vcpu_get_tsc_scaling_frac_bits, NULL, "%llu\n");

/*
 * A private GPA is backed by restrictedmem, while a shared GPA uses the
 * Firecracker userspace HVA.  Restrictedmem is not freed by a
 * private-to-shared transition, so physical_backing_bytes deliberately sums
 * both backing domains.
 */
struct faasev_memory_backing {
	u64 hva_resident_bytes;
	u64 hva_private_bytes;
	u64 hva_shared_bytes;
	u64 hva_mixed_bytes;
	struct xarray pfns;
	u64 alias_bytes;
	int error;
};

struct faasev_hva_walk {
	struct kvm *kvm;
	struct kvm_memory_slot *slot;
	struct faasev_memory_backing *backing;
};

/* Only the explicit audit read enumerates PFNs; the workload uses counters. */
static void faasev_audit_pfn(struct faasev_memory_backing *backing,
                           unsigned long pfn)
{
	void *old = xa_store(&backing->pfns, pfn, xa_mk_value(1), GFP_ATOMIC);

	if (xa_is_err(old))
		backing->error = xa_err(old);
	else if (old)
		backing->alias_bytes += PAGE_SIZE;
}

static void faasev_account_hva_leaf(struct faasev_hva_walk *state,
				    unsigned long addr, unsigned long next,
				    unsigned long pfn, bool audit)
{
	struct faasev_memory_backing *backing = state->backing;
	unsigned long bytes = next - addr;
	gfn_t gfn = state->slot->base_gfn +
		(addr - state->slot->userspace_addr) / PAGE_SIZE;
	gfn_t end = gfn + bytes / PAGE_SIZE;
	bool private = kvm_mem_is_private(state->kvm, gfn);
	bool mixed = false;
	gfn_t gfn_cursor;

	for (gfn_cursor = gfn + 1; gfn_cursor < end; gfn_cursor++) {
		if (kvm_mem_is_private(state->kvm, gfn_cursor) != private) {
			mixed = true;
			break;
		}
	}

	if (audit) {
		unsigned long i;
		for (i = 0; i < bytes / PAGE_SIZE; i++)
			faasev_audit_pfn(backing, pfn + i);
	}
	backing->hva_resident_bytes += bytes;
	if (mixed)
		backing->hva_mixed_bytes += bytes;
	else if (private)
		backing->hva_private_bytes += bytes;
	else
		backing->hva_shared_bytes += bytes;
}

static void faasev_walk_hva_range(struct faasev_hva_walk *state,
				  unsigned long start, unsigned long end, bool audit)
{
	struct mm_struct *mm = state->kvm->mm;
	unsigned long addr = start;

	while (addr < end) {
		pgd_t *pgd = pgd_offset(mm, addr);
		unsigned long pgd_next = pgd_addr_end(addr, end);
		p4d_t *p4d;
		pud_t *pud;
		unsigned long pud_next;

		if (pgd_none(*pgd) || pgd_bad(*pgd)) {
			addr = pgd_next;
			continue;
		}
		p4d = p4d_offset(pgd, addr);
		if (p4d_none(*p4d) || p4d_bad(*p4d)) {
			addr = pgd_next;
			continue;
		}
		pud = pud_offset(p4d, addr);
		pud_next = pud_addr_end(addr, end);
		if (pud_none(*pud)) {
			addr = pud_next;
			continue;
		}
		if (pud_trans_huge(*pud) || pud_devmap(*pud)) {
			spinlock_t *ptl = pud_lock(mm, pud);
			pud_t value = *pud;

			if (pud_present(value) &&
			    (pud_trans_huge(value) || pud_devmap(value)))
				faasev_account_hva_leaf(state, addr, pud_next,
					pud_pfn(value) + ((addr & ~PUD_MASK) >> PAGE_SHIFT), audit);
			spin_unlock(ptl);
			addr = pud_next;
			continue;
		}

		if (pud_bad(*pud)) {
			addr = pud_next;
			continue;
		}

		while (addr < pud_next) {
			pmd_t *pmd = pmd_offset(pud, addr);
			unsigned long pmd_next = pmd_addr_end(addr, pud_next);

			if (pmd_none(*pmd)) {
				addr = pmd_next;
				continue;
			}
			if (pmd_trans_huge(*pmd) || pmd_devmap(*pmd)) {
				spinlock_t *ptl = pmd_lock(mm, pmd);
				pmd_t value = *pmd;

				if (pmd_present(value) &&
				    (pmd_trans_huge(value) || pmd_devmap(value)))
					faasev_account_hva_leaf(state, addr, pmd_next,
					pmd_pfn(value) + ((addr & ~PMD_MASK) >> PAGE_SHIFT), audit);
				spin_unlock(ptl);
				addr = pmd_next;
				continue;
			}

			if (pmd_bad(*pmd)) {
				addr = pmd_next;
				continue;
			}

			{
				spinlock_t *ptl;
				pte_t *pte = pte_offset_map_lock(mm, pmd, addr, &ptl);

				while (addr < pmd_next) {
					pte_t value = *pte;

					if (pte_present(value) &&
					    !is_zero_pfn(pte_pfn(value)))
						faasev_account_hva_leaf(state, addr,
									addr + PAGE_SIZE, pte_pfn(value), audit);
					pte++;
					addr += PAGE_SIZE;
				}
				pte_unmap_unlock(pte - 1, ptl);
			}
			cond_resched();
		}
	}
}

static void faasev_account_hva_residency(struct kvm *kvm,
					  struct faasev_memory_backing *backing)
{
	struct kvm_memslots *slots;
	struct kvm_memory_slot *slot;
	int bkt;
	int idx;

	idx = srcu_read_lock(&kvm->srcu);
	mmap_read_lock(kvm->mm);
	slots = kvm_memslots(kvm);
	kvm_for_each_memslot(slot, bkt, slots) {
		struct faasev_hva_walk state = {
			.kvm = kvm,
			.slot = slot,
			.backing = backing,
		};
		unsigned long start = slot->userspace_addr;
		unsigned long end = start + slot->npages * PAGE_SIZE;

		if (!slot->npages || slot->flags & KVM_MEMSLOT_INVALID ||
		    kvm_is_error_hva(start) || end < start)
			continue;
		faasev_walk_hva_range(&state, start, end, true);
	}
	mmap_read_unlock(kvm->mm);
	srcu_read_unlock(&kvm->srcu, idx);
}

static int kvm_faasev_memory_backing_show(struct seq_file *m, void *v)
{
	struct kvm *kvm = m->private;
	struct faasev_memory_backing backing = {};
	u64 restricted_backing = 0;
	struct kvm_memory_slot *slot;
	int bkt, idx;

	xa_init(&backing.pfns);
	idx = srcu_read_lock(&kvm->srcu);
	kvm_for_each_memslot(slot, bkt, kvm_memslots(kvm)) {
		gfn_t gfn;

		if (!kvm_slot_can_be_private(slot))
			continue;
		for (gfn = slot->base_gfn; gfn < slot->base_gfn + slot->npages; gfn++) {
			kvm_pfn_t pfn;
			int ret = kvm_restrictedmem_get_pfn_noalloc(slot, gfn, &pfn, NULL);

			if (!ret) {
				faasev_audit_pfn(&backing, pfn);
				restricted_backing += PAGE_SIZE;
				put_page(pfn_to_page(pfn));
			} else if (ret != -ENOENT) {
				backing.error = ret;
			}
		}
		cond_resched();
	}
	srcu_read_unlock(&kvm->srcu, idx);

	faasev_account_hva_residency(kvm, &backing);

	seq_printf(m, "restricted_backing_bytes %llu\n", restricted_backing);
	seq_printf(m, "restricted_counter_bytes %lld\n",
		   atomic64_read(&kvm->faasev_restricted_private_bytes));
	seq_printf(m, "alias_bytes %llu\n", backing.alias_bytes);
	seq_printf(m, "hva_resident_bytes %llu\n", backing.hva_resident_bytes);
	seq_printf(m, "hva_private_bytes %llu\n", backing.hva_private_bytes);
	seq_printf(m, "hva_shared_bytes %llu\n", backing.hva_shared_bytes);
	seq_printf(m, "hva_mixed_bytes %llu\n", backing.hva_mixed_bytes);
	seq_printf(m, "physical_backing_bytes %llu\n",
		   restricted_backing + backing.hva_resident_bytes);
	xa_destroy(&backing.pfns);
	return backing.error;
}

/* HugeTLB workload: no full-RAM page walk and no allocation on read. */
static int kvm_faasev_memory_fast_show(struct seq_file *m, void *v)
{
	struct kvm *kvm = m->private;
	struct mm_struct *mm = kvm->mm;
	struct faasev_memory_backing small = {};
	struct kvm_memory_slot *slot, *other;
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, 0);
	u64 guest_huge_span = 0, mm_huge_span = 0, private_small = 0;
	u64 hva_huge, restricted;
	int bkt, other_bkt, err = 0;
	int idx = srcu_read_lock(&kvm->srcu);

	mmap_read_lock(mm);
	/* exit_mmap may precede closing the KVM fd and removing its debugfs. */
	if (!atomic_read(&mm->mm_users)) {
		err = -ENOENT;
		goto out;
	}
	for_each_vma(vmi, vma)
		if (is_vm_hugetlb_page(vma))
			mm_huge_span += vma->vm_end - vma->vm_start;
	kvm_for_each_memslot(slot, bkt, kvm_memslots(kvm)) {
		unsigned long start = slot->userspace_addr;
		unsigned long end = start + (slot->npages << PAGE_SHIFT);
		unsigned long addr = start;
		struct faasev_hva_walk walk = { kvm, slot, &small };

		if (!slot->npages || slot->flags & KVM_MEMSLOT_INVALID)
			continue;
		if (kvm_is_error_hva(start) || end < start) {
			err = -EINVAL;
			goto out;
		}
		/* A process-wide HugeTLB counter is valid only for disjoint RAM mappings. */
		kvm_for_each_memslot(other, other_bkt, kvm_memslots(kvm)) {
			if (other == slot || !other->npages)
				continue;
			if (start < other->userspace_addr + (other->npages << PAGE_SHIFT) &&
			    other->userspace_addr < end) {
				err = -EINVAL;
				goto out;
			}
		}
		while (addr < end) {
			unsigned long next;

			vma = find_vma(mm, addr);
			if (!vma || vma->vm_start > addr) {
				err = -EINVAL;
				goto out;
			}
			next = min(end, vma->vm_end);
			if (is_vm_hugetlb_page(vma)) {
				if (!IS_ALIGNED(addr, PMD_SIZE) || !IS_ALIGNED(next, PMD_SIZE)) {
					err = -EINVAL;
					goto out;
				}
				guest_huge_span += next - addr;
			} else {
				gfn_t gfn = slot->base_gfn + ((addr - start) >> PAGE_SHIFT);
				gfn_t last = gfn + ((next - addr) >> PAGE_SHIFT);

				/* Workload's only small RAM region is GPA [0, 2 MiB). */
				if (last > PMD_SIZE / PAGE_SIZE) {
					err = -EOPNOTSUPP;
					goto out;
				}
				faasev_walk_hva_range(&walk, addr, next, false);
				for (; gfn < last; gfn++) {
					kvm_pfn_t pfn;
					int ret = kvm_restrictedmem_get_pfn_noalloc(slot, gfn, &pfn, NULL);

					if (!ret) {
						private_small += PAGE_SIZE;
						put_page(pfn_to_page(pfn));
					} else if (ret != -ENOENT) {
						err = ret;
						goto out;
					}
				}
			}
			addr = next;
		}
	}
	if (!guest_huge_span || mm_huge_span != guest_huge_span) {
		/* Memslots are installed before private launch pages are populated. */
		err = atomic64_read(&kvm->faasev_restricted_private_bytes) ?
			-EINVAL : -EAGAIN;
		goto out;
	}
	hva_huge = atomic_long_read(&mm->hugetlb_usage) << PAGE_SHIFT;
	restricted = atomic64_read(&kvm->faasev_restricted_private_bytes);
	if (restricted < private_small) {
		err = -EINVAL;
		goto out;
	}
	seq_printf(m, "restricted_backing_bytes %llu\n", restricted);
	seq_printf(m, "restricted_hugetlb_bytes %llu\n", restricted - private_small);
	seq_printf(m, "hva_hugetlb_bytes %llu\n", hva_huge);
	seq_printf(m, "hva_small_bytes %llu\n", small.hva_resident_bytes);
	seq_printf(m, "physical_backing_bytes %llu\n",
		   restricted + hva_huge + small.hva_resident_bytes);
out:
	mmap_read_unlock(mm);
	srcu_read_unlock(&kvm->srcu, idx);
	return err;
}

static int kvm_faasev_memory_fast_open(struct inode *inode, struct file *file)
{
	struct kvm *kvm = inode->i_private;
	int ret;

	if (!kvm_get_kvm_safe(kvm))
		return -ENOENT;
	ret = single_open(file, kvm_faasev_memory_fast_show, kvm);
	if (ret)
		kvm_put_kvm(kvm);
	return ret;
}

static int kvm_faasev_memory_backing_open(struct inode *inode, struct file *file)
{
	struct kvm *kvm = inode->i_private;
	int ret;

	if (!kvm_get_kvm_safe(kvm))
		return -ENOENT;
	ret = single_open(file, kvm_faasev_memory_backing_show, kvm);
	if (ret)
		kvm_put_kvm(kvm);
	return ret;
}

static int kvm_faasev_memory_backing_release(struct inode *inode,
					      struct file *file)
{
	kvm_put_kvm(inode->i_private);
	return single_release(inode, file);
}

static const struct file_operations kvm_faasev_memory_backing_fops = {
	.open = kvm_faasev_memory_backing_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = kvm_faasev_memory_backing_release,
};

static const struct file_operations kvm_faasev_memory_fast_fops = {
	.open = kvm_faasev_memory_fast_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = kvm_faasev_memory_backing_release,
};


static int severifast_profile_show(struct seq_file *m, void *v)
{
	struct kvm *kvm = m->private;
	struct severifast_profile *p = kvm->arch.severifast_profile;
	unsigned long i;

	if (!p)
		return -ENODATA;
	seq_printf(m, "rmp_ns %lld\nrmp_4k %lld\nrmp_2m %lld\n",
		   atomic64_read(&p->rmp_ns), atomic64_read(&p->rmp_4k),
		   atomic64_read(&p->rmp_2m));
	if (p->access[0] && !p->phase) {
		for (i = 0; i < p->bins; i++)
			seq_printf(m, "access%s %lu %d %d\n", p->access_shift ? "" : "4k", i,
				   test_bit(i, p->access[0]) ? 1 : 0,
				   test_bit(i, p->access[1]) ? 1 : 0);
	}
	return 0;
}

static int severifast_profile_open(struct inode *inode, struct file *file)
{
	struct kvm *kvm = inode->i_private;
	int ret;

	if (!kvm_get_kvm_safe(kvm))
		return -ENOENT;
	ret = single_open(file, severifast_profile_show, kvm);
	if (ret)
		kvm_put_kvm(kvm);
	return ret;
}

static ssize_t severifast_profile_write(struct file *file,
			const char __user *buf, size_t len, loff_t *offset)
{
	struct kvm *kvm = ((struct seq_file *)file->private_data)->private;
	struct severifast_profile *p;
	struct kvm_memory_slot *slot;
	struct kvm_memslots *slots;
	int iter;
	char command[16];
	int ret = 0;

	if (!len || len >= sizeof(command))
		return -EINVAL;
	if (copy_from_user(command, buf, len))
		return -EFAULT;
	command[len] = 0;
	mutex_lock(&kvm->slots_lock);
	p = kvm->arch.severifast_profile;
	if (!strcmp(command, "timing") || !strcmp(command, "access") ||
	    !strcmp(command, "access-4k")) {
		if (p) { ret = -EEXIST; goto out; }
		if (!tdp_mmu_enabled || atomic_read(&kvm->online_vcpus) != 1) { ret = -EINVAL; goto out; }
		p = kzalloc(sizeof(*p), GFP_KERNEL);
		if (!p) { ret = -ENOMEM; goto out; }
		if (strcmp(command, "timing")) {
			p->access_shift = !strcmp(command, "access") ? 9 : 0;
			slots = kvm_memslots(kvm);
			kvm_for_each_memslot(slot, iter, slots)
			    if (slot->id < KVM_USER_MEM_SLOTS)
				p->bins = max_t(unsigned long, p->bins,
					DIV_ROUND_UP(slot->base_gfn + slot->npages, 1UL << p->access_shift));
			p->access[0] = bitmap_zalloc(p->bins, GFP_KERNEL);
			p->access[1] = bitmap_zalloc(p->bins, GFP_KERNEL);
			if (!p->access[0] || !p->access[1]) {
				bitmap_free(p->access[0]); bitmap_free(p->access[1]); kfree(p);
				ret = -ENOMEM; goto out;
			}
		}
		WRITE_ONCE(kvm->arch.severifast_profile, p);
	} else if (p && p->access[0]) {
		if (!strcmp(command, "init") && p->phase == 0)
			kvm_severifast_access_boundary(kvm, 1);
		else if (!strcmp(command, "invoke") && p->phase == 1)
			kvm_severifast_access_boundary(kvm, 2);
		else if (!strcmp(command, "finish") && p->phase == 2)
			kvm_severifast_access_boundary(kvm, 0);
		else
			ret = -EINVAL;
	} else {
		ret = -EINVAL;
	}
out:
	mutex_unlock(&kvm->slots_lock);
	return ret ? ret : len;
}

static const struct file_operations severifast_profile_fops = {
	.open = severifast_profile_open,
	.read = seq_read,
	.write = severifast_profile_write,
	.llseek = seq_lseek,
	.release = kvm_faasev_memory_backing_release,
};

void kvm_arch_create_vcpu_debugfs(struct kvm_vcpu *vcpu, struct dentry *debugfs_dentry)
{
	debugfs_create_file("guest_mode", 0444, debugfs_dentry, vcpu,
			    &vcpu_guest_mode_fops);
	debugfs_create_file("tsc-offset", 0444, debugfs_dentry, vcpu,
			    &vcpu_tsc_offset_fops);

	if (lapic_in_kernel(vcpu))
		debugfs_create_file("lapic_timer_advance_ns", 0444,
				    debugfs_dentry, vcpu,
				    &vcpu_timer_advance_ns_fops);

	if (kvm_caps.has_tsc_control) {
		debugfs_create_file("tsc-scaling-ratio", 0444,
				    debugfs_dentry, vcpu,
				    &vcpu_tsc_scaling_fops);
		debugfs_create_file("tsc-scaling-ratio-frac-bits", 0444,
				    debugfs_dentry, vcpu,
				    &vcpu_tsc_scaling_frac_fops);
	}
}

/*
 * This covers statistics <1024 (11=log(1024)+1), which should be enough to
 * cover RMAP_RECYCLE_THRESHOLD.
 */
#define  RMAP_LOG_SIZE  11

static const char *kvm_lpage_str[KVM_NR_PAGE_SIZES] = { "4K", "2M", "1G" };

static int kvm_mmu_rmaps_stat_show(struct seq_file *m, void *v)
{
	struct kvm_rmap_head *rmap;
	struct kvm *kvm = m->private;
	struct kvm_memory_slot *slot;
	struct kvm_memslots *slots;
	unsigned int lpage_size, index;
	/* Still small enough to be on the stack */
	unsigned int *log[KVM_NR_PAGE_SIZES], *cur;
	int i, j, k, l, ret;

	if (!kvm_memslots_have_rmaps(kvm))
		return 0;

	ret = -ENOMEM;
	memset(log, 0, sizeof(log));
	for (i = 0; i < KVM_NR_PAGE_SIZES; i++) {
		log[i] = kcalloc(RMAP_LOG_SIZE, sizeof(unsigned int), GFP_KERNEL);
		if (!log[i])
			goto out;
	}

	mutex_lock(&kvm->slots_lock);
	write_lock(&kvm->mmu_lock);

	for (i = 0; i < kvm_arch_nr_memslot_as_ids(kvm); i++) {
		int bkt;

		slots = __kvm_memslots(kvm, i);
		kvm_for_each_memslot(slot, bkt, slots)
			for (k = 0; k < KVM_NR_PAGE_SIZES; k++) {
				rmap = slot->arch.rmap[k];
				lpage_size = kvm_mmu_slot_lpages(slot, k + 1);
				cur = log[k];
				for (l = 0; l < lpage_size; l++) {
					index = ffs(pte_list_count(&rmap[l]));
					if (WARN_ON_ONCE(index >= RMAP_LOG_SIZE))
						index = RMAP_LOG_SIZE - 1;
					cur[index]++;
				}
			}
	}

	write_unlock(&kvm->mmu_lock);
	mutex_unlock(&kvm->slots_lock);

	/* index=0 counts no rmap; index=1 counts 1 rmap */
	seq_printf(m, "Rmap_Count:\t0\t1\t");
	for (i = 2; i < RMAP_LOG_SIZE; i++) {
		j = 1 << (i - 1);
		k = (1 << i) - 1;
		seq_printf(m, "%d-%d\t", j, k);
	}
	seq_printf(m, "\n");

	for (i = 0; i < KVM_NR_PAGE_SIZES; i++) {
		seq_printf(m, "Level=%s:\t", kvm_lpage_str[i]);
		cur = log[i];
		for (j = 0; j < RMAP_LOG_SIZE; j++)
			seq_printf(m, "%d\t", cur[j]);
		seq_printf(m, "\n");
	}

	ret = 0;
out:
	for (i = 0; i < KVM_NR_PAGE_SIZES; i++)
		kfree(log[i]);

	return ret;
}

static int kvm_mmu_rmaps_stat_open(struct inode *inode, struct file *file)
{
	struct kvm *kvm = inode->i_private;
	int r;

	if (!kvm_get_kvm_safe(kvm))
		return -ENOENT;

	r = single_open(file, kvm_mmu_rmaps_stat_show, kvm);
	if (r < 0)
		kvm_put_kvm(kvm);

	return r;
}

static int kvm_mmu_rmaps_stat_release(struct inode *inode, struct file *file)
{
	struct kvm *kvm = inode->i_private;

	kvm_put_kvm(kvm);

	return single_release(inode, file);
}

static const struct file_operations mmu_rmaps_stat_fops = {
	.open		= kvm_mmu_rmaps_stat_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= kvm_mmu_rmaps_stat_release,
};

int kvm_arch_create_vm_debugfs(struct kvm *kvm)
{
	debugfs_create_file("severifast_profile", 0600, kvm->debugfs_dentry,
			    kvm, &severifast_profile_fops);
	debugfs_create_file("mmu_rmaps_stat", 0644, kvm->debugfs_dentry, kvm,
			    &mmu_rmaps_stat_fops);
	debugfs_create_file("faasev_memory_backing_bytes", 0444,
			    kvm->debugfs_dentry, kvm, &kvm_faasev_memory_backing_fops);
	debugfs_create_file("faasev_memory_backing_fast", 0400, kvm->debugfs_dentry,
			    kvm, &kvm_faasev_memory_fast_fops);
	return 0;
}
