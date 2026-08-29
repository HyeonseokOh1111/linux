# AGENTS.md — Linux guest helper-baseline rules

## Role

- Linux guest는 VMPL0 direct boot, snapshot trigger, software CPU-context capture와 restore continuation을 담당한다.
- Active helper path는 `faasev_vmpl=1` lower-VMPL direct boot나 VMSA-only continuation을 사용하지 않는다. Guest-visible Linux vCPU와 VMPL0 hidden worker는 서로 다른 KVM vCPU다.
- Artifact path, attestation response, page payload 선택과 lazy-fault I/O는 Recorder, Restorer와 Firecracker가 담당한다. Linux에 snapshot file이나 working-set policy를 넣지 않는다.
- FaaSEV 변경은 snapshot/restore와 input-key runtime 경로에 제한하고 일반 VMPL0 SNP boot와 표준 Linux behavior를 불필요하게 바꾸지 않는다. 공통 규칙은 `../../AGENTS.md`를 따른다.

## Snapshot trigger and CPU-context ABI

- `snapshot_enter` syscall은 current task를 boot CPU에 pin하고 shadow-crypto/AP rendezvous/virtio freeze 같은 sleep 가능한 준비를 마친 뒤 IRQ와 preemption을 끄고 BSP kernel continuation, CPU context와 extents를 publish해 host capture VMGEXIT를 보낸다. Restored BSP는 syscall 내부의 `restore_continue`로 돌아와 IRQ/preemption과 affinity를 복구하고 0을 반환한다. Snapshot 전용 user-return hook은 없다.
- CPU context는 Linux-defined software structure다. Saved kernel GPR/RIP/RSP/RFLAGS continuation, restore mailbox address, FS/GS bases, CR0/3/4, EFER, fixed-size GDT/XSAVE copy, XCR0/PKRU, restore entry/page와 IRQ-stack state처럼 native continuation이 실제 소비하는 값만 포함한다. IDT는 field로 저장하지 않고 per-CPU state 복구 뒤 `load_current_idt()`로 설치하며 TSS selector와 GDT size는 Linux fixed layout에서 직접 계산한다.
- CPU context에 `context_size`, `valid_flags`, segment selector, CR2, XSAVE length, unused restore-stub RIP와 LDT/TR copy처럼 사용되지 않거나 fixed layout에서 파생되는 field를 두지 않는다. Required state를 완전히 capture하지 못하면 partial context나 flag 조합을 publish하지 말고 snapshot capture를 실패시킨다.
- CPU bundle은 CPU-index 순서의 fixed-size context 배열과 `N > 1`일 때 바로 뒤따르는 restore barrier만 포함한다. 별도 mailbox header와 topology 배열을 두지 않으며 CPU 수, layout과 길이는 launch/control contract와 fixed structure size에서 계산한다.
- Single-vCPU mode는 CPU0 하나만 capture한다. Online CPU 수가 2 이상이면 online CPU가 연속된 `0..N-1`인지 확인하고 per-CPU kernel thread로 secondary CPU context를 rendezvous한다.
- Guest-visible vCPU 수는 1–64다. CPU 수는 existing launch/control contract에서 얻고 CPU identity는 배열 index로 계산한다. Artifact length와 launch vCPU 수가 fixed layout에 맞지 않으면 fallback 없이 실패한다.
- CPU context와 shared extents는 14–16 MiB private mailbox에 publish한다. KVM/vCPU state, virtio state와 JSON host replay artifact를 별도로 만들지 않는다.

## Page-state notification

- Linux shared extent의 유일한 source는 final capture 시점의 active C-bit mapping scan이다. 일반 RAM은 direct map을, encrypted direct-map alias를 가진 kernel `.bss..decrypted` page는 active kernel-image mapping을 검사한다. Virtio와 secondary CPU를 quiesce한 뒤 빈 mailbox output에 연속 shared range를 직접 coalesce하며 capacity 초과는 snapshot capture 실패다.
- PSC, GHCB, SWIOTLB와 virtio-ring allocation에는 snapshot 전용 incremental tracking hook이나 known-range seed를 두지 않는다. 이 SEV path는 `.init.scratch`를 shared로 전환하지 않으므로 free/reuse 전에 별도 private re-transition을 추가하지 않는다.
- Linux는 `page_macs.bin`, mixed-page bitmap과 restore payload를 해석하지 않는다.

## Snapshot quiesce

- Snapshot 전에 virtio device를 기존 freeze callback으로 정지하고 current task affinity, tick, IRQ/preemption과 per-CPU capture rendezvous를 정해진 순서로 quiesce한다.
- Capture 실패는 이미 frozen된 virtio와 affinity를 복구하고 error를 반환한다. Partial CPU bundle을 snapshot-ready로 publish하지 않는다.
- CPU0이 final context를 publish하고 final active-mapping scan으로 shared extents를 mailbox에 publish한 뒤 host capture signal을 보내며 fresh execution에서는 capture 이후 spin한다.
- Multi-vCPU secondary CPU는 capture signal 이후 fresh Snapshot-CVM에서 영구 대기하고, restored AP는 saved capture-function `restore_continue`로 돌아온다.

## Restore continuation

- Snapshot Restorer는 private mailbox에 verified CPU bundle과 shared immutable landing page 한 장을 설치한 뒤 guest-visible BSP/AP vCPU를 Linux restore entry로 handoff한다. Landing은 저장된 kernel continuation stack pointer 아래의 scratch 영역을 Linux C handoff stack으로 사용한다. BSP와 AP capture는 Restorer와 같은 `SP - 0x800`, 16-byte alignment 및 8-byte sentinel 산식의 최종 RSP가 current task stack의 usable 범위 안인지 확인한 뒤에만 context bundle을 publish한다. Linux는 restored TSS, runtime #VC IDT와 GHCB를 활성화하고 나머지 runtime/device restore를 수행하며, 저장된 hardirq stack은 normal interrupt entry가 사용하도록 pointer와 in-use state만 복구한다.
- Linux는 launch/control path가 제공한 CPU 수로 raw bundle의 fixed offsets를 계산하고 자신의 CPU index에 해당하는 context를 선택한다. 제거한 header, topology 또는 validity flags를 다른 이름으로 재도입하지 않는다.
- Restore는 GDT/TR, standard current IDT, control/base registers와 XCR0/XSAVE/PKRU를 복구한다. FS/GS 복구 뒤 `setup_ghcb_for_snapshot_restore()`가 authenticated snapshot의 negotiated protocol version을 재사용하고, fresh Template launch의 per-CPU runtime GHCB page를 MSR PSC로 shared 전환한 뒤 등록한다. 그 다음 runtime #VC IDT를 활성화하고 표준 KVM/PV clock, local APIC과 `tick_resume_local()`을 재사용해 BSP/AP 공통 kernel continuation resume 하나로 이어진다. Direct user-mode restore와 return-mode 분기는 없다. CR4는 Restorer landing에서 설치하고 Linux restore entry가 다시 쓰지 않는다.
- 각 CPU가 CPU-local 복구 뒤 `arrived`를 publish한다. BSP는 모든 AP를 기다린 뒤 virtio/BSP-global state를 복구하고 `resume`을 publish한다. Input private key는 BSP continuation에서 설치하지 않으며, 수동 local-APIC timer programming은 사용하지 않고 `tick_resume_local()`을 canonical 경로로 사용한다.
- Multi-vCPU restore barrier는 `arrived`와 `resume` 두 field만 사용한다. AP-local state를 global virtio restore 뒤로 미루거나 BSP의 resume publication을 앞당기지 않는다.
- Lazy page fault의 artifact 선택과 page installation은 host/Firecracker/Restorer가 담당하며 Linux fault path에 file I/O나 payload verification을 넣지 않는다.

## Input-key and runtime ABI

- Restore firmware는 decrypted input private key를 14–16 MiB private mailbox의 fixed 32-byte one-shot slot에 저장한다. Slot은 최대 CPU bundle과 shared landing page에서 분리하며 input key bytes만 담는다.
- Restored runner는 `snapshot_enter`가 0을 반환한 직후 input을 받거나 workload를 실행하기 전에 `faasev_seed_input_key(NULL, 0)`를 호출한다. Kernel은 nonzero mailbox slot을 한 번만 소비해 persistent `gcm(aes)` transform에 설치하고 slot과 temporary buffer를 지우며 syscall 실패 시 runner는 진행하지 않는다. 이미 비워진 slot의 재소비는 실패한다.
- Normal path는 in-guest normal attestation helper가 같은 `faasev_seed_input_key(user_key, 32)` syscall을 호출한다. 두 mode는 같은 common transform-install 함수에 도달하지만 Restore key는 user memory로 복사하지 않는다.
- Input private key를 secrets page, snapshot artifact, shared transport 또는 user-visible log에 기록하지 않는다. Restore mailbox slot은 snapshot page artifact나 CPU bundle field가 아니다.
- `faasev_shadow_crypto`, mailbox key offset, seed syscall 번호·두 입력 mode와 key length를 바꾸면 Restorer producer, runner와 normal-attestation-helper consumer를 같은 작업에서 수정한다.

## I/O and boot contract

- Snapshot 전후 virtio device는 existing driver freeze/restore callbacks으로 정지·복구한다.
- Guest rootfs와 funcfs는 partition table 없이 whole-disk virtio-blk device를 사용하고 dm-verity roothash는 boot cmdline/config contract를 따른다.
- SWIOTLB는 표준 4 KiB 경로만 사용하고 helper 전용 hugepage option이나 allocation mode를 두지 않는다. dm-verity 검증은 유지하되 verify-path 세부 timing marker는 만들지 않는다. Host fine-grained parser에 남은 verity field는 모두 0인 output-shape placeholder이며 측정값으로 해석하지 않는다.
- Snapshot initramfs는 `snapshot_init.sh`와 `init=/bin/fc_init` path만 사용한다.
- Required kernel config, cmdline, syscall table, header와 build wiring은 관련 기능 변경과 같은 작업에서 갱신한다.

## Kernel integration

- Snapshot customization은 `sev_snapshot.c`, existing SNP helpers, KVM guest reinit과 virtio callbacks에 국소화한다.
- General MM/page-fault path를 restore policy의 첫 구현 위치로 사용하지 않는다. Host memory-fault exit와 trusted helper transport로 해결할 수 없는 경우에만 kernel fault hook을 검토한다.
- Shared/private transition, `PVALIDATE`, GHCB registration과 SNP guest request는 기존 AMD helper boundary를 사용한다.
- Multi-vCPU restore helper를 바꾸면 CPU capture, manifest parser, Restorer AP entry와 Firecracker resume order를 함께 검증한다.
- 사용되지 않는 mailbox metadata, AP fallback과 VMSA path를 speculative하게 추가하지 않는다.

## Validation

- Guest kernel 변경 뒤에는 intended config, snapshot initramfs/cmdline boot, single-vCPU capture/restore와 변경 영향이 있으면 multi-vCPU path를 검증한다.
- Snapshot/restore ABI 변경은 old snapshot을 stale로 취급하고 snapshot 생성, working-set 기록, restore 순서로 새 artifact를 만든다.
- Input-key 또는 virtio 변경은 normal과 restore 양쪽을 검증한다.
- MM, entry assembly, CPU context와 IRQ-stack 변경은 focused reproduction과 marker/dmesg correlation을 남긴다.
