/* Private VMPL1/VMPL0 ABI; mirrored by page_state_contract.rs. */
#ifndef _ASM_X86_FAASEV_SERVICE_H
#define _ASM_X86_FAASEV_SERVICE_H

#include <linux/types.h>
#include <linux/compiler.h>

#define FAASEV_SERVICE_DOORBELL 0xf500000000000014ULL
#define FAASEV_SINGLE_MAILBOX_BASE 0x00e00000UL
#define FAASEV_SERVICE_GPA (FAASEV_SINGLE_MAILBOX_BASE + 0x1000UL)
#define FAASEV_SERVICE_PENDING 1
#define FAASEV_SERVICE_SUCCESS 2
#define FAASEV_SERVICE_MAKE_PRIVATE 1
#define FAASEV_SERVICE_MAKE_SHARED 2
#define FAASEV_SERVICE_PREPARE_PRIVATE 3

struct faasev_vmpl0_service_page {
	u64 sequence;
	u32 opcode;
	u32 status;
	u64 gpa;
	u64 npages;
};

static __always_inline bool faasev_service_publish(struct faasev_vmpl0_service_page *svc,
					 u32 opcode, u64 gpa, u64 npages)
{
	u64 sequence = READ_ONCE(svc->sequence);

	if (sequence == ~0ULL || READ_ONCE(svc->status) == FAASEV_SERVICE_PENDING)
		return false;
	WRITE_ONCE(svc->gpa, gpa);
	WRITE_ONCE(svc->npages, npages);
	WRITE_ONCE(svc->opcode, opcode);
	WRITE_ONCE(svc->sequence, sequence + 1);
	barrier();
	WRITE_ONCE(svc->status, FAASEV_SERVICE_PENDING);
	barrier();
	return true;
}

static __always_inline bool faasev_service_finish(struct faasev_vmpl0_service_page *svc)
{
	bool ok;

	barrier();
	ok = READ_ONCE(svc->status) == FAASEV_SERVICE_SUCCESS;
	WRITE_ONCE(svc->gpa, 0);
	WRITE_ONCE(svc->npages, 0);
	WRITE_ONCE(svc->opcode, 0);
	WRITE_ONCE(svc->status, 0);
	/* Keep the sequence across boot stages and GHCB reinitialization. */
	barrier();
	return ok;
}
#endif
