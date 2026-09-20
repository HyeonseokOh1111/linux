# AGENTS.md — Linux host KVM rules

## Role

- 이 component는 SNP launch, restricted memory, memslot/NPT/RMP 일관성, memory attribute와 GHCB/VMGEXIT dispatch를 담당한다.
- Page payload 선택, snapshot MAC 검증, working set과 workflow 의미는 Firecracker와 VMPL0 Recorder/FaaSEV firmware가 담당한다. Host kernel에 semantic page server나 artifact policy를 넣지 않는다.
- 공통 단순화, ABI 변경과 검증 원칙은 `../../AGENTS.md`를 따르며 이 문서에 반복하지 않는다.

## Memory and fault contract

- `memfd_restricted(RESTRICTEDMEM_HUGETLB)`는 workload에서만 사용하는 opt-in 경로다. 2 MiB HugeTLB backing은 요청된 페이지에 대해 demand allocation하며 전체 file 크기를 선예약하지 않는다. HugeTLB cache index와 KVM의 4 KiB page offset을 변환하고, 기존 invalidation·migration 금지·RMP 해제를 유지한다. Pool 부족 시 shmem fallback하지 않는다. 이 지원은 built-in `mm/restrictedmem.c`에 있으므로 KVM 모듈 재로드만으로 적용할 수 없다.

- 일반 memory-attribute 변경은 이미 존재하는 restrictedmem page에만 적용하고 sparse range 전체를 즉시 materialize하지 않는다. 실제 RMP fault나 PSC가 요청한 page만 demand allocation한다.
- Lower-VMPL memory fault는 current occurrence의 missing-page installation을 위해 userspace FaaSEV 경로로 전달한다. Private/shared transition은 명시적 PSC 경로만 사용하며 backing 존재 여부를 나타내는 custom memory-fault flag를 두지 않는다. 이미 current target에 설치된 page가 FaaSEV memory-fault 경로로 들어오면 fail-stop한다.
- Lower-VMPL의 명시적인 private/shared page-state 전환은 memory-fault 경로와 구분하여 기존 GHCB PSC 또는 VMPL0 service request 경로로만 처리한다.
- 현재 VMPL을 알 수 없는 fault를 VMPL1로 간주하지 않는다. VMPL0, VMPL1-3과 unknown 상태를 명시적으로 구분하고 unknown은 fail-stop한다.
- Memory-attribute 요청의 2 MiB private RMPUPDATE는 정렬과 backing 조건을 만족하고 userspace가 `KVM_MEMORY_ATTRIBUTE_FAASEV_ALLOW_HUGE_PRIVATE`를 명시한 범위에만 사용한다. 그 밖의 범위는 기존 4 KiB 경로를 따른다.
- HugeTLB 및 THP의 demand fault에는 host module option `snp_huge_private`를 별도로 사용할 수 있다. 기본값은 false이며, true일 때도 restrictedmem이 실제 order 9 backing을 반환하고 저메모리 밖의 정렬된 전체 2 MiB 범위가 같은 memslot 안에 있으며 모든 subpage가 private·unassigned인 경우에만 2 MiB로 등록한다. 4 KiB backing, mixed·부분·이미 등록된 범위의 처리는 바꾸지 않는다. 이 경로는 전체 RAM을 미리 할당하지 않는다. Workload wrapper는 HugeTLB 실행 동안 이를 켜고 복구하며, 일반 Single 실험에서 사용할 때도 실험 전 값을 저장하고 종료 뒤 복구한다.
- SNP guest-request 응답이 HugeTLB shared backing 위에 있으면 PSP에는 별도의 4 KiB page를 전달하고 reclaim 후 ciphertext를 guest 응답 page로 복사한다. Host의 HugeTLB mapping 안에 PSP 전용 private subpage를 만들지 않는다. Reclaim 실패 시 firmware 소유 page는 반환하지 않고 원래 guest 응답 page의 pin만 해제한다.
- RMP, memslot과 NPT ordering 및 기존 KVM lock의 serialization 의미를 바꾸지 않는다.

## Change boundary

- Non-SEV KVM과 일반 SNP guest 동작을 바꾸지 않고 FaaSEV hook을 최소 범위에 둔다.
- Fault path에는 blocking I/O, payload copy, 불필요한 allocation과 semantic verification을 추가하지 않는다.
- KVM UAPI flag나 memory-encryption operation을 바꾸면 같은 작업에서 Firecracker producer/consumer를 함께 수정한다. 프로젝트 자체 UAPI에 compatibility version, fallback이나 사용되지 않는 field를 추가하지 않는다.
- `restrictedmem` 내부 구조를 외부 header에 노출하지 않고, 할당 없는 조회처럼 필요한 최소 helper만 제공한다.

## Workload memory accounting

- `faasev_memory_backing_fast`는 HugeTLB workload의 실제 restricted backing과 HVA backing을 집계한다. Process의 HugeTLB VMA가 겹치지 않는 guest RAM memslot에만 속하는지 확인한 뒤 기존 `mm->hugetlb_usage`를 사용하고, GPA `[0,2 MiB)`의 작은 backing만 별도로 조회한다. 전체 RAM 순회나 VM 정지를 주기 수집에 넣지 않는다.
- `faasev_memory_backing_bytes`의 상세 조회는 측정 전 검증에 사용한다. 실제 restricted page와 HVA page를 열거해 allocation counter와 대조하고 물리 PFN 중복을 보고한다. 이 상세 검사를 본 workload의 주기 수집에서 호출하지 않는다.
- Private/shared 상태만 바뀌고 backing이 남아 있으면 실제 점유량에 계속 포함한다. Pool 잔여 사용량과 VM별 집계의 차이를 누락하거나 자동으로 누수라고 단정하지 않는다.
