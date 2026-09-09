# AGENTS.md — Linux guest rules

## Role

- Linux guest는 `faasev_vmpl=1` measured direct boot, snapshot trigger와 VMSA continuation만 담당한다. Fresh boot는 VMPL1에서 수행하고 Heap continuation은 같은 snapshot을 VMPL1-3 중 선택된 lower VMPL에서 실행할 수 있다.
- Artifact, attestation과 FaaSEV page policy는 Recorder, VMPL0 FaaSEV firmware와 Firecracker가 담당한다. Linux에 workflow, artifact path나 page payload 선택을 넣지 않는다.
- FaaSEV 변경은 lower-VMPL 경로에만 제한하고 일반 VMPL0 SNP boot와 표준 Linux helper 동작을 바꾸지 않는다. 공통 규칙은 `../../AGENTS.md`를 따른다.

## VMPL1 direct boot

- Compressed boot와 decompressed main-kernel early boot는 각각 진입 시 `faasev_vmpl=1`을 한 번 판정하고, 각 stage의 이후 경로는 해당 stage에 저장된 early flag를 재사용한다.
- VMPL1은 `enforce_vmpl0()`를 건너뛰고 firmware가 제공한 E820, ACPI RSDP와 boot parameters를 사용하며 BIOS, ROM과 EBDA를 별도로 탐색하지 않는다.
- VMPL1은 private/shared page를 직접 PSC·`PVALIDATE`하지 않고 VMPL0 Recorder에 요청한다. Initrd, SWIOTLB, decrypted BSS, fresh page와 Linux가 동적 할당한 real-mode trampoline도 실제 접근 전에 준비를 요청한다.
- GHCB 통신 확인 signal만 유지하고 boot phase별 debug VMGEXIT를 두지 않는다.

## Page-state notification

- Linux는 shared extent의 복사본을 저장하지 않고 자신이 발생시킨 4 KiB/2 MiB private/shared 전환 범위만 Recorder에 통지한다.
- Authoritative shared-state 추적과 snapshot page 판정은 `../snapshot-recorder/AGENTS.md`를 따른다. Linux에 mirror table, direct-map rebuild, fallback이나 동기화 검증을 추가하지 않는다.

## Snapshot and continuation

- `snapshot_enter()`는 single vCPU/CPU0에서 virtio를 freeze하고 tick, IRQ와 preemption을 정지한 뒤 snapshot VMGEXIT를 보낸다. 현재 N>1은 fallback 없이 unsupported로 종료하며 multi-vCPU snapshot/FaaSEV 지원은 future work로 남긴다.
- FaaSEV continuation은 저장된 lower-VMPL VMSA의 snapshot VMGEXIT 다음 instruction에서 이어진다. GHCB를 재등록하고 표준 Linux/KVM helper로 clock과 tick을 복구한 뒤 IRQ/preemption, input private key, virtio와 task affinity를 복구한다.
- Linux는 CPU-state descriptor나 FaaSEV payload를 파싱하지 않고 별도 FaaSEV entry로 reboot하거나 AP/multi-vCPU 복구 경로를 두지 않는다.
- Input private key는 `../snapshot-restorer/AGENTS.md`에 따라 VMPL0 FaaSEV firmware가 준비한 private mailbox에서 실행 재개 후 읽어 설치하고 임시 buffer를 즉시 zeroize한다. Virtio 복구는 IRQ-disabled atomic 구간을 나온 뒤 수행한다.

## Heap guest ABI

- Object-heap layout, bank ownership과 VMPL 권한은 `../firecracker/AGENTS.md`와 `../snapshot-restorer/AGENTS.md`를 따른다.
- Linux는 고정 object-heap mapping과 route-aware input/output 조회·commit만 VMPL0 service로 요청한다. Query response는 `route`, `ptr`, `len`만 가지며 `NETWORK`에서는 pointer와 length가 0이다. Output query만 optional bounded outcome을 전달하고 input query는 outcome을 전달하지 않는다.
- Runner는 `HEAP` input/output에만 object bank를 사용한다. `NETWORK` input은 shadow input을 읽고, `NETWORK` output은 shadow output을 먼저 publish한 뒤 zero length로 commit한다. Commit은 output length와 같은 outcome을 전달하고 terminal success/failure를 반환받는다.
- Fanout/fanin scheduling과 child completion은 Linux ABI에 추가하지 않는다. Lower VMPL function은 route query와 output commit만 수행하고 VMPL0 FaaSEV firmware와 host control plane이 authenticated graph를 처리한다.

## I/O and runtime ABI

- Snapshot 전후 virtio device는 기존 driver의 freeze/restore callback으로 정지·복구한다.
- SWIOTLB buffer와 metadata는 접근 전에 VMPL0에 private-page 준비를 요청하며 hugepage mode는 2 MiB 정렬 범위만 사용한다.
- Guest rootfs는 partition table 없이 whole-disk virtio-blk device를 직접 사용한다. `dm-verity` read verification 시간 측정은 실행 경로를 변경하지 않는 관측 기능으로만 유지한다.
- `snapshot_enter` syscall은 snapshot trigger만 담당하고 CPU state, artifact와 FaaSEV plan을 Linux ABI에 추가하지 않는다.
- Input-key 설치와 shadow-crypto 실행은 현재 syscall과 private mailbox 계약만 사용하며 key를 snapshot artifact와 shared memory에 저장하지 않는다.
- Heap syscall은 object-heap mapping, input/output bank 조회와 output commit으로 제한한다.

## Kernel integration

- Early page-table, memblock, initrd, SWIOTLB, decrypted memory와 page allocation 변경은 VMPL1 direct boot에서 실제 접근 전 private page를 준비하는 최소 범위로 제한한다.
- Lower-VMPL missing-page `#VC`는 Linux에서 `PVALIDATE`하거나 payload를 설치하지 않고 같은 instruction을 재시도한다. Host lazy page installation 순서는 `../firecracker/AGENTS.md`의 Lazy page installation 및 `../snapshot-restorer/AGENTS.md`의 FaaSEV contract를 따른다.
- FaaSEV continuation에서 clock, KVM per-vCPU state, tick과 IRQ stack은 표준 kernel helper를 사용해 복구하며 수동 LAPIC programming, broad PV reinitialization과 multi-vCPU/AP 복구 helper를 추가하지 않는다.
- Syscall table, header, build wiring과 producer/consumer ABI 선언은 해당 기능과 함께 변경하고 사용되지 않는 entry, offset과 include를 남기지 않는다.
