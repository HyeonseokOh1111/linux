/* SPDX-License-Identifier: GPL-2.0 */
#ifndef KVM_SEVERIFAST_PROFILE_H
#define KVM_SEVERIFAST_PROFILE_H

#include <linux/kvm_host.h>
#include <linux/bitmap.h>

/* Optional per-VM motivation measurements. One bit per 2 MiB or 4 KiB. */
struct severifast_profile {
	atomic64_t rmp_ns;
// FAASEV_EVAL_DISABLED 	atomic64_t rmp_shared_ns;
// FAASEV_EVAL_DISABLED 	atomic64_t psmash_ns;
	atomic64_t rmp_4k;
	atomic64_t rmp_2m;
	unsigned int phase;
	unsigned int access_shift;
	unsigned long bins;
	unsigned long *access[2];
};

static inline void severifast_record_access(struct kvm *kvm, gfn_t gfn,
					   int level)
{
	struct severifast_profile *p = READ_ONCE(kvm->arch.severifast_profile);
	unsigned long first, end, i;
	unsigned int phase;

	if (!p || !(phase = READ_ONCE(p->phase)))
		return;
	first = gfn >> p->access_shift;
	end = min_t(unsigned long, p->bins,
		DIV_ROUND_UP(gfn + (1UL << (9 * (level - 1))), 1UL << p->access_shift));
	for (i = first; i < end; i++)
		set_bit(i, p->access[phase - 1]);
}

void kvm_severifast_access_boundary(struct kvm *kvm, unsigned int phase);

#endif
