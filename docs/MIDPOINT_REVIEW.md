# Design++ Phase 0~5 중간점검

점검일: 2026-08-24

## 1. 점검 방법

이 문서는 `PROJECT_PLAN.md`의 Phase 0~5를 현재 소스, 테스트 목록, 실제 빌드와
WSL 통합 실행 결과에 대조한다. 구현 파일이 존재한다는 사실만으로 완료로
판정하지 않았다. 자동 테스트 또는 실제 실행 증거가 부족한 항목은 `부분 완료`
또는 `미검증`으로 남겼다.

이번 점검에서 사용한 검증 결과는 다음과 같다.

- x64 Debug 별도 출력 전체 컴파일·링크: 성공, 경고 0개
- x64 Release solution 전체 컴파일·링크: 성공, 경고 0개
- C++ 단위·계약·GUI helper 테스트: 128/128 통과
- 실제 WSL/OpenSTA 통합 테스트: clean/setup violation/hold violation 3/3 통과
- `git diff --check`: 오류 없음

기본 Debug 출력은 실행 중인 `Design++.exe`가 잠겨 있어 링크만 실패했다. 같은
Debug 설정을 `x64/DebugAudit`에 링크해 코드와 설정 자체의 성공을 확인했다.

## 2. 단계별 판정

| 단계 | 판정 | 확인된 증거 | 남은 핵심 작업 |
|---|---|---|---|
| Phase 0 기반 | 완료 | Job Object process runner, 구조화 WSL command, x64 빌드와 process 계약 테스트 | 없음 |
| Phase 1 도구/리소스 | 부분 완료 | bounded scheduler, CPU token, cross-process quota와 quota 테스트 | Toolchain Profile, PDK root 검증, Doctor, fairness/shutdown 고부하 행렬 |
| Phase 2 Library/Workspace | 핵심 완료 | Library schema v3, Project schema v4 migration/round-trip, writer lease, atomic managed source와 다중 View factory 테스트 | 최근 프로젝트 목록과 더 강한 다중 프로세스 contention 테스트 |
| Phase 3 Lint | vertical slice 완료 | Verilator adapter, diagnostics, source navigation, execution exactly-once 테스트 | LSP/고급 편집 기능은 명시적 후속 범위 |
| Phase 4 Simulation | 부분 완료 | Icarus Run/Debug, VCD artifact, GTKWave 실행, Verilator/FST와 cocotb adapter, xUnit parser | Verilator/cocotb Workspace UI 연결, test configuration UI, test summary 집계 |
| Phase 5 Yosys | vertical slice 완료 | 15개 synthesis service 계약 테스트, Yosys adapter/fixture, Readable/Gate schematic, router/canvas 성능 테스트, 실제 Yosys 통합 fixture | 대형 사용자 회로 수동 smoke 결과를 정식 release checklist로 기록 |
| Phase 5 OpenSTA | 실행 vertical slice 완료, 안정화 부분 완료 | 호환 synthesis fingerprint, managed SDC/Liberty, OpenSTA adapter/service, 실제 2.6.0 분석, timing summary v2 | 아래 4절의 안정화 gate |

## 3. 이번 다듬기 결과

- Timing 화면을 텍스트 덤프에서 요약 영역과 column 기반 violation 표로 분리했다.
- WNS/TNS와 violation slack에 `ns` 단위를 명시했다.
- setup, hold, recovery, removal을 따로 집계한다.
- recovery는 max/setup 결과에, removal은 min/hold 결과에 반영한다.
- Problems 코드는 `TIMING-SETUP`, `TIMING-HOLD`, `TIMING-RECOVERY`,
  `TIMING-REMOVAL`로 구분한다.
- timing summary schema v2가 개별 check 수를 보존한다.
- Nix shell 경고는 raw log에는 남지만 OpenSTA Problems로 승격하지 않는다.

## 4. Phase 5를 완전히 동결하기 전 필수 안정화 gate

우선순위 순서다. 이 항목을 마치기 전에는 Phase 5 전체를 최종 완료로 부르지
않는다.

1. `TimingRunService` 실패 행렬을 synthesis service 수준으로 확장한다.
   probe/start/nonzero, 필수 artifact 누락·빈 파일, malformed marker/report,
   staging 실패, finalize cancellation, stale generation, shutdown 및 재시작
   복원을 포함한다.
2. Timing Runs를 실제 표로 바꾸고 corner, setup/hold WNS/TNS, status를 표시한
   뒤 선택한 과거 Run의 summary, violation, artifacts를 복원한다.
3. 숨김 `TimingWindow` smoke test로 factory 분리, Run/Cancel 상태, 버튼,
   과거 Run 선택 및 message responsiveness를 검증한다.
4. 한글·공백 Library 경로에서 OpenSTA staging과 artifact 복사를 실제 WSL
   통합 테스트로 고정하고 recovery/removal fixture를 추가한다.

## 5. 다음 개발 순서

1. 위 Phase 5 안정화 gate
2. Phase 4 backlog 중 test configuration UI와 Verilator/cocotb Workspace 연결
3. 전체 Phase 0~5 release smoke checklist 실행 및 결과 기록
4. 그 후 Phase 6 OpenLane 2 managed-flow vertical slice 시작

현재 상태에서 OpenSTA 엔진 자체는 실제 회로를 분석하고 위반을 올바르게
표시한다. 다음 단계의 위험은 분석 알고리즘보다 실패·복원 계약과 Run history
UX에 집중돼 있다.
