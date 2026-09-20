# AGENTS.md — Linux guest rules

## Role

- Linux guest는 `faasev_vmpl=1` measured direct boot, snapshot trigger와 VMPL1 VMSA continuation을 담당한다.
- Artifact, attestation과 FaaSEV page policy는 Recorder, VMPL0 FaaSEV firmware와 Firecracker가 담당한다. Linux에 workflow, artifact path나 page payload 선택을 넣지 않는다.
- FaaSEV 변경은 lower-VMPL 경로에만 제한하고 일반 VMPL0 SNP boot와 표준 Linux helper 동작을 바꾸지 않는다. 공통 규칙은 `../../AGENTS.md`를 따른다.

## VMPL1 direct boot

- Guest config는 `CONFIG_PHYSICAL_START=0x1000000`, `CONFIG_RELOCATABLE=y`, `CONFIG_PHYSICAL_ALIGN=0x1000000`을 사용한다. Kernel은 16 MiB, input-key mailbox는 14 MiB에 고정한다.
- Service page는 mailbox base + `(CPU + 1) * 4 KiB`에 두고 direct map 생성 전에 memblock에 추가·예약한다. Firmware E820 예약과 key page read-only 권한을 유지한다. CPU0만 지원하고 초과 CPU는 거부한다.
- SME 전용 `.init.scratch`는 SEV 경로에서 사용하지 않으며 kernel reserve 밖의 재사용 가능한 RAM이다. Device가 할당된 뒤 이 주소 범위를 다시 private으로 변환하지 않는다.

- Compressed boot와 decompressed main-kernel early boot는 각각 진입 시 `faasev_vmpl=1`을 한 번 판정하고, 각 stage의 이후 경로는 해당 stage에 저장된 early flag를 재사용한다.
- VMPL1은 `enforce_vmpl0()`를 건너뛰고 firmware가 제공한 E820, ACPI RSDP와 boot parameters를 사용하며 BIOS, ROM과 EBDA를 별도로 탐색하지 않는다.
- VMPL1은 private/shared page를 직접 PSC·`PVALIDATE`하지 않고 VMPL0 Recorder에 요청한다. Initrd, SWIOTLB, decrypted BSS와 Linux가 동적 할당한 real-mode trampoline도 실제 접근 전에 준비를 요청한다.
- GHCB 통신 확인 signal만 유지하고 boot phase별 debug VMGEXIT를 두지 않는다.

## Page-state notification

- Linux는 shared extent의 복사본을 저장하지 않고 자신이 발생시킨 4 KiB/2 MiB private/shared 전환 범위만 Recorder에 통지한다.
- Authoritative shared-state 추적과 snapshot page 판정은 `../snapshot-recorder/AGENTS.md`를 따른다. Linux에 mirror table, direct-map rebuild, fallback이나 동기화 검증을 추가하지 않는다.

## Snapshot and continuation

- `snapshot_enter()`는 single vCPU/CPU0에서 virtio를 freeze하고 tick, IRQ와 preemption을 정지한 뒤 snapshot VMGEXIT를 보낸다. 현재 N>1은 fallback 없이 unsupported로 종료하며 multi-vCPU snapshot/FaaSEV 지원은 future work로 남긴다.
- FaaSEV continuation은 저장된 lower-VMPL VMSA의 snapshot VMGEXIT 다음 instruction에서 이어진다. GHCB를 재등록하고 표준 Linux/KVM helper로 clock과 fresh Template의 local APIC를 초기화하고 tick을 복구한 뒤 IRQ/preemption, input private key, virtio와 task affinity를 복구한다.
- IRQ/preemption 복구 뒤 RTC가 복원된 wall clock보다 앞서 있으면 표준 timekeeping helper로 wall clock을 갱신하여 snapshot 경과 시간 때문에 서명된 network request가 만료되지 않게 한다.
- Linux는 CPU-state descriptor나 FaaSEV payload를 파싱하지 않고 별도 FaaSEV entry로 reboot하거나 AP/multi-vCPU 복구 경로를 두지 않는다.
- Input private key는 `../snapshot-restorer/AGENTS.md`에 따라 VMPL0 FaaSEV firmware가 준비한 private mailbox에서 실행 재개 후 읽어 설치하고 임시 buffer를 즉시 zeroize한다. Virtio 복구는 IRQ-disabled atomic 구간을 나온 뒤 수행한다.

## I/O and runtime ABI

- Snapshot 전후 virtio device는 기존 driver의 freeze/restore callback으로 정지·복구한다.

- SWIOTLB buffer와 metadata는 접근 전에 VMPL0에 private-page 준비를 요청하며 hugepage mode는 2 MiB 정렬 범위만 사용한다.
- Guest rootfs는 partition table 없이 whole-disk virtio-blk device를 직접 사용한다. `dm-verity` read verification 시간 측정은 실행 경로를 변경하지 않는 관측 기능으로만 유지한다.
- `snapshot_enter` syscall은 snapshot trigger만 담당하고 CPU state, artifact와 FaaSEV plan을 Linux ABI에 추가하지 않는다.
- Input-key 설치와 shadow-crypto 실행은 현재 syscall과 private mailbox 계약만 사용하며 key를 snapshot artifact와 shared memory에 저장하지 않는다.

## Kernel integration

- Early page-table, memblock, initrd, SWIOTLB와 decrypted memory 변경은 VMPL1 direct boot에서 실제 접근 전 private page를 준비하는 최소 범위로 제한한다.
- 일반 page allocation에는 VMPL0 준비 요청 hook을 두지 않는다. Direct boot RAM은 Recorder가 lower VMPL 실행 전에 준비하고, FaaSEV continuation의 missing page는 기존 lazy page installation 경로에서 준비한다. 명시적인 private/shared 전환과 service buffer 준비는 유지한다.
- Lower-VMPL missing-page `#VC`는 Linux에서 `PVALIDATE`하거나 payload를 설치하지 않고 같은 instruction을 재시도한다. Host lazy page installation 순서는 `../firecracker/AGENTS.md`의 Lazy page installation 및 `../snapshot-restorer/AGENTS.md`의 FaaSEV contract를 따른다.
- FaaSEV continuation에서 clock, KVM per-vCPU state, tick과 IRQ stack은 표준 kernel helper를 사용해 복구하며 수동 LAPIC programming, broad PV reinitialization과 multi-vCPU/AP 복구 helper를 추가하지 않는다.
- Syscall table, header, build wiring과 producer/consumer ABI 선언은 해당 기능과 함께 변경하고 사용되지 않는 entry, offset과 include를 남기지 않는다.
