# AGENTS.md — Guest kernel repository

## What this repository is

This tree should be treated as a **Linux guest kernel with SEV-SNP support**, likely still close to upstream structure.
As of the current checkout, the exact upstream diff is not reliable because the local `.git` state is broken, so patch history should be described conservatively.

That means this repository should be approached with an **upstream-style discipline**:
- prefer minimal, isolated changes,
- avoid making the kernel the first home for system-level orchestration,
- keep Linux MM and SNP semantics as standard as possible unless there is a strong measured reason to deviate.

---

## Current checkout facts that must guide edits

### The tree looks close to standard Linux SNP layout
Important files are the usual SNP-related kernel areas, including:
- `mem_encrypt_amd.c`
- `sev.h`
- `fault.c`
- `sev-guest.c`
- `uapi/sev-guest.h`

This suggests the kernel side is not currently the most heavily custom part of the project.

### The kernel already provides core SNP primitives
Relevant SNP-facing primitives include:
- `pvalidate`
- `rmpadjust`
- `snp_set_memory_shared/private`
- `snp_issue_guest_request`

There is also the `/dev/sev-guest` guest-request ABI with ioctls such as:
- `SNP_GET_REPORT`
- `SNP_GET_DERIVED_KEY`
- `SNP_GET_EXT_REPORT`

These are the right places to understand the kernel’s SNP boundary.
Do not duplicate their semantics elsewhere casually.

### The page-fault path still looks standard
The current MM fault flow is the standard Linux path:
- `do_user_addr_fault()`
- `handle_mm_fault()`

SNP page-state transitions are also managed through the standard AMD memory-encryption helpers, such as the prepare/finish status-change path and hypercall notifier path.

That means the kernel does **not** currently look like it already contains a custom page-granular restore subsystem.

### Kernel hooking should be the last resort
Given the current source shape, if page-granular restore is needed, the first preferred integration point is not `fault.c`.
It is the combination of:
- trusted-runtime communication,
- Firecracker communication buffer / runtime exit handling,
- host/guest restore contract design.

Kernel hooks should come only after those routes are clearly insufficient.

### Kernel config is part of the contract
Important config expectations include:
- `CONFIG_BLK_DEV_INITRD=y`
- `CONFIG_AMD_MEM_ENCRYPT=y`
- `CONFIG_KVM_GUEST=y`
- `CONFIG_VIRTIO_BLK=y`
- `CONFIG_VIRTIO_NET=y`
- `CONFIG_SEV_GUEST=m`

In the referenced Lupine config, `CONFIG_VSOCKETS` is disabled.

Do not treat config drift as harmless.
Boot, initramfs, and guest-request behavior depend on it.

### Handoff from initramfs is cmdline-driven
The initramfs-to-kernel contract in this project is primarily through cmdline choices such as:
- `init=/bin/fc_init`
- `rdinit=/bin/fc_init`
- legacy `rdinit=/bin/myinit`

This means some behavior that looks “kernel-related” is actually owned by top-level boot/orchestration, not by kernel internal code.

---

## What changes belong here

Good fits for this repository:
- small, necessary SNP-related fixes,
- driver/config changes needed by the guest environment,
- narrowly scoped changes to support a well-defined host/guest restore contract,
- additions that preserve upstream-style semantics,
- observability hooks only when existing layers truly cannot expose the needed signal.

Bad fits for this repository:
- first-choice implementation of page-granular restore policy,
- system-level restore orchestration,
- initramfs policy,
- host/guest mailbox protocol definition,
- attestation-server logic,
- broad experimental behavior inserted straight into `fault.c` without exhausting higher-level options.

---

## Design rules

### 1) Keep kernel changes minimal and surgical
Because the tree appears close to stock Linux SNP support, large invasive changes should be avoided unless strictly necessary.
A smaller patch surface is easier to maintain, review, and eventually upstream-align.

### 2) Respect existing SNP helper boundaries
If a change touches:
- shared/private transitions,
- page validation,
- guest requests,
- memory-encryption state changes,

route it through the existing AMD SNP helper paths where possible.
Do not create parallel semantics.

### 3) Treat `fault.c` as a last resort
Page faults are tempting integration points, but they are also high-risk.
Before modifying fault handling, ask whether the same goal can be achieved with:
- trusted runtime protocol,
- Firecracker runtime exits,
- communication buffers,
- explicit restore descriptors.

### 4) Keep config compatibility explicit
If a feature depends on a kernel config option, document that dependency and update the config source of truth.
Do not rely on a local build environment happening to set the option.

### 5) Preserve the boundary between kernel code and boot policy
If the real change is about `init=` / `rdinit=` / initramfs behavior, it probably belongs in top-level wiring or initramfs scripts, not in kernel internals.

### 6) Keep UAPI stable and deliberate
If touching `/dev/sev-guest` or related UAPI:
- treat it as API,
- version or gate changes carefully,
- update all consumers,
- avoid project-local shortcuts that would make the interface ambiguous.

### 7) Prefer observability over policy in the kernel
If you need the kernel involved, prefer adding a narrow hook or trace point over embedding full restore policy into the kernel.

---

## Implementation guidance

### MM and page-fault changes
Only modify MM/page-fault logic when:
- a higher-level host/trusted-runtime route truly cannot solve the problem,
- the required behavior is precisely specified,
- the performance/security tradeoff is measured and documented.

If fault-path code is touched, keep the patch as small and well-commented as possible.

### SNP state transitions
Changes to shared/private transitions, `pvalidate`, or related helpers must preserve existing Linux SNP assumptions.
Use the existing prepare/finish and helper boundaries where possible.

### Guest-request ABI
For report/key-related kernel interactions, prefer the existing `/dev/sev-guest` ABI instead of inventing project-local control paths inside the kernel.

### Drivers and config
When adding or changing guest functionality, first check whether the right answer is:
- enabling/configuring an existing driver,
- adjusting initramfs behavior,
- or changing top-level cmdline/config generation,

rather than patching kernel core code.

### Handoff assumptions
Remember that initramfs handoff is cmdline-driven in this project.
Do not move policy into the kernel if the real source of truth is the boot cmdline.

---

## What not to do

- Do not treat the kernel as the default home for snapshot/restore orchestration.
- Do not jump into `fault.c` first just because page-fault restore sounds kernel-like.
- Do not duplicate SNP helper semantics outside the established helper paths.
- Do not change `/dev/sev-guest` behavior casually.
- Do not ignore config dependencies.
- Do not bury boot-policy decisions in kernel code when they are really cmdline/initramfs issues.
- Do not describe the tree as heavily customized unless that is actually verified.

---

## Minimum validation after changes

At minimum:
- ensure the intended kernel config still enables the required SNP and device features,
- ensure the kernel still boots under the existing initramfs/cmdline contract,
- ensure `/dev/sev-guest` behavior remains compatible if touched,
- ensure any MM/SNP-path changes are tested in the relevant boot/restore scenario.

If a change touches MM or page-fault logic, add a focused regression or at least a documented reproduction path.

---

## Success criteria

A change in this repository is in good shape when:
- it keeps the kernel close to standard SNP/Linux behavior,
- it solves only the part that truly belongs in kernel space,
- higher-level orchestration is left to trusted runtime / Firecracker / initramfs where appropriate,
- config and boot contracts stay explicit,
- and the patch remains maintainable.
