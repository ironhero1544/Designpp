# Design++ 제작 계획

## Tool Check 검증 현황 (2026-09-07)

2026-09-08 Tool Check 수정: managed 환경 inventory가 모든 기존 checkout에
무조건 `환경 준비 필요`를 반환하던 임시 동작을 제거했다. 명시적 준비가
성공한 환경은 provider/version/commit 완료 표식을 남기며, 일반 검사는 Nix
shell을 열지 않고 표식과 실제 checkout commit을 read-only로 검증한다.
Determinate Nix의 `nix (Determinate Nix ...) <version>` 출력도 정상 파싱한다.
OpenLane 설치는 2.3.10 commit을 후보 디렉터리에서 검증한 뒤 원자적으로
활성화하고, ORFS와 동일하게 실패 시 기존 환경을 보존한다.

2026-09-08 추가 검증: 승인된 후보 소스 빌드를 완료했다. Yosys 레시피가
배포판 ABC를 잘못 선택하던 부분을 고정 ABC 서브모듈로 교체하고, GoogleTest
1.14.0을 해시 고정 입력으로 공급했다. `read_lib -m`와
`repair_timing -sequence` 검사가 통과했다. EQY도 commit
`eff96db01293848b993651caa52d747f191be02e`로 고정하고 같은 Yosys에 대해
빌드했다. nangate45와 sky130hd 공개 GCD fixture 모두 최종
GDS/DEF/ODB/netlist/SDC 생성 및 headless ODB load까지 확인했다. sky130hd의
post-CTS equivalence check도 비활성화하지 않고 통과했다.

ORFS 26Q2 후보는 annotated tag ID가 아닌 실제 commit
`036d106273e66855cd5214d49518fd0f0df7de61`로 고정한다. 설치는 해당
OpenROAD/Yosys submodule commit도 검사하며, 기존 설치와 실패 후보를 보존한다.
일반 ORFS probe/Run은 offline/cache-only로 실행하고 설치도 승인 없는 소스
빌드를 차단한다. 실제 후보 준비에서 OpenROAD 바이너리 캐시 부재로 이 차단이
동작했으며, 별도 승인된 소스 빌드로 Nix closure를 준비한 뒤 두 platform의
full-flow를 검증했다. 공통 inventory/preparation
서비스, 승인 UI, 설치 manifest 및 rollback UI 통합도 별도 미완료 항목이다.

## Layout Setup 안정화 계약

Setup은 Cell별 draft를 소유하고 빈 값으로 override를 해제한다. 폼 Save와
JSON Ctrl+S는 같은 비동기 저장 경로를 사용하며 저장 후 창을 유지한다.
schema v11은 자동값 선택을 보존한다. 전체 완료 판정에는 실제 Save 버튼과
디스크 재로드, 저장 실패/종료 경합, 두 Cell 격리, Monaco 수명, ORFS 자동 및
고정 면적 Floorplan integration 검증이 필요하다.

## 1. 프로젝트 목표

Design++는 파편화된 오픈소스 디지털 설계 도구를 하나의 Windows 데스크톱
프로그램에서 설정하고 실행하며 결과를 추적할 수 있게 하는 EDA Flow
Orchestrator다.

Design++ 자체가 합성, STA, 배치·배선 알고리즘을 다시 구현하지는 않는다.
OpenLane 2와 OpenROAD Flow Scripts(ORFS)를 managed backend로 사용하고,
Verilator, Yosys, OpenSTA 등 개별 도구는 standalone adapter로 연결한다.

기본 설계 흐름은 다음과 같다.

```text
프로젝트 설정
    ↓
RTL 입력
    ↓
Lint
    ↓
시뮬레이션
    ↓
합성
    ↓
STA
    ↓
Floorplan
    ↓
Placement
    ↓
CTS
    ↓
Routing
    ↓
DRC / LVS
    ↓
GDSII / 리포트
```

## 2. 확정 기술 방향

| 항목 | 결정 |
|---|---|
| 언어 | C++20 |
| GUI | 순수 Win32 API |
| 빌드 | Visual Studio, MSBuild, `.slnx`/`.vcxproj` |
| Windows 대상 | x64 우선, Win32 구성 유지 |
| EDA 실행 환경 | WSL2 Linux |
| 프로세스 실행 | `wsl.exe --exec` 기반 구조화된 명령 |
| 병렬 실행 | CPU token 기반 bounded multicore scheduler |
| 다중 실행 | 여러 창과 여러 Design++ 프로세스 동시 실행 지원 |
| 전체 Flow backend | OpenLane 2, ORFS |
| 프로젝트 파일 | Cell별 UTF-8 JSON `.dpproj` schema v10 (v1~v9 읽기 호환) |
| 외부 라이브러리 | 사전 승인 없이 추가하지 않음 |

CMake와 Qt는 사용하지 않는다. GUI는 Windows에서 네이티브로 실행되고 실제
EDA 프로그램은 WSL2 안에서 실행한다.

## 3. 도구 구성

| 목적 | 프로그램 | Design++에서의 역할 |
|---|---|---|
| SystemVerilog 검증 | Verilator | 기본 Lint 및 고속 시뮬레이션 |
| 간단한 Verilog 시뮬레이션 | Icarus Verilog | 학습 및 호환 시뮬레이션 옵션 |
| Python 테스트벤치 | cocotb | Verilator/Icarus test runner |
| 파형 확인 | GTKWave | VCD/FST 외부 Viewer |
| 합성 | Yosys | standalone 합성 및 managed flow 내부 합성 |
| STA | OpenSTA | standalone timing 분석 및 managed flow 내부 STA |
| Physical Design | OpenROAD | Floorplan, Placement, CTS, Routing |
| DRC/LVS | Magic, Netgen | 물리 검증 |
| Layout 확인 | KLayout | GDSII, LEF/DEF 외부 Viewer |
| 전체 RTL-to-GDS | OpenLane 2 | 기본 turnkey flow backend |
| 고급 RTL-to-GDS | ORFS | 단계별 실행과 물리 설계 제어 backend |

## 4. 제품 실행 모드

### 4.1 Managed Flow

실제 RTL-to-GDS 프로젝트를 진행하는 모드다.

- 한 번의 run은 OpenLane 2 또는 ORFS 중 하나가 소유한다.
- Design++는 설정 생성, 실행, 로그, 진행 상태, 결과와 메트릭을 통합한다.
- backend 고유 checkpoint와 resume 방식을 존중한다.
- backend 내부 스크립트와 디렉터리 구조를 임의로 수정하지 않는다.
- 초기 버전에서는 서로 다른 backend의 중간 산출물을 한 run 안에서 혼합하지 않는다.

OpenLane 2의 Step/State/Flow 구조는 Design++의 Stage/Artifact/Run 모델과
매핑한다. ORFS는 외부 `DESIGN_CONFIG`를 전달하는 방식으로 연결하며 ORFS를
포크하지 않는다.

참고:

- [OpenLane 2 Architecture](https://openlane2.readthedocs.io/en/stable/reference/architecture.html)
- [OpenROAD Flow Scripts](https://openroad-flow-scripts.readthedocs.io/en/latest/mainREADME.html)

### 4.2 Standalone Tool Flow

학습, 빠른 RTL 검증, 특정 단계 디버깅을 위한 모드다.

- Verilator lint/simulation
- Icarus simulation
- cocotb test
- Yosys synthesis
- OpenSTA timing analysis
- GTKWave/KLayout/OpenROAD GUI 실행

Standalone 결과를 managed flow에 전달하려면 PDK, timing corner, 단위,
constraint 및 파일 형식 호환성을 검증해야 한다.

## 5. 전체 아키텍처

```text
┌────────────────────────────────────────────────────────────┐
│                    Native Win32 GUI                        │
│ Project Explorer / Flow / Inspector / Log / Metrics       │
└──────────────────────────┬─────────────────────────────────┘
                           │
┌──────────────────────────▼─────────────────────────────────┐
│ Core                                                       │
│ Project / Flow Graph / Stage / Artifact / Run / Metrics   │
└───────────────┬──────────────────────────┬─────────────────┘
                │                          │
┌───────────────▼─────────────┐  ┌────────▼──────────────────┐
│ Tool / Flow Adapters        │  │ Run & Artifact Store      │
│ Verilator, OpenLane, ORFS…  │  │ manifest, log, report     │
└───────────────┬─────────────┘  └───────────────────────────┘
                │
┌───────────────▼────────────────────────────────────────────┐
│ Win32 Runtime                                              │
│ Scheduler / ProcessRunner / Job Object / WslExecutor      │
└───────────────┬────────────────────────────────────────────┘
                │
┌───────────────▼────────────────────────────────────────────┐
│ WSL2 Linux                                                 │
│ OpenLane 2 / ORFS / Verilator / Yosys / OpenROAD / PDK   │
└────────────────────────────────────────────────────────────┘
```

의존 방향과 현재 런타임 세부사항은 [ARCHITECTURE.md](ARCHITECTURE.md)를
참고한다.

## 6. 핵심 도메인 모델

### 6.1 Project

프로젝트는 최소한 다음 정보를 가진다.

- 프로젝트 이름
- top module
- RTL source/file list
- include directory와 macro define
- parameter
- clock/reset
- SDC 및 추가 constraint
- testbench/cocotb 설정
- PDK와 platform
- managed flow backend
- WSL2/toolchain profile

### 6.2 Stage

각 Stage adapter는 다음 책임을 가진다.

```text
probe       설치 여부와 버전 확인
validate    프로젝트 설정과 입력 artifact 검증
plan        실행할 구조화된 명령 생성
execute     runtime 계층에 실행 요청
parse       로그, 결과, 메트릭 정규화
openResult  적절한 외부 viewer 실행
```

초기 adapter는 정적으로 등록한다. 동적 C++ DLL plugin은 ABI와 toolchain
호환성 문제가 있으므로 v1 범위에서 제외한다. 외부 plugin이 필요해지면
프로세스 분리형 JSON IPC 방식을 검토한다.

### 6.3 Artifact

다음 산출물을 타입으로 관리한다.

- RTL set
- Netlist
- SDC
- Liberty/LEF set
- DEF
- SPEF/SDF
- VCD/FST waveform
- GDSII
- DRC/LVS report
- 일반 log/report

각 artifact에는 생성 도구와 버전, PDK fingerprint, 설정 hash, 입력 hash를
기록한다. RTL 또는 제약이 바뀌면 영향을 받는 이후 단계를 `Stale`로
표시한다.

### 6.4 Run 상태

```text
Pending → Queued → Running → Succeeded
                         ├→ Failed
                         ├→ Cancelled
                         └→ Interrupted
```

앱이 종료되거나 비정상 종료되어도 다음 실행 시 run 상태와 로그를 복구할 수
있어야 한다.

## 7. WSL2 실행 규칙

Windows GUI가 Linux 도구 command line을 직접 문자열로 결합하지 않는다.
모든 실행은 다음 정보를 가진 구조체로 표현한다.

```text
distribution
linuxWorkingDirectory
program
arguments[]
environment[]
```

runtime 계층은 이를 다음 형태로 변환한다.

```text
wsl.exe [--distribution DISTRO] [--cd DIR] --exec \
  [/usr/bin/env KEY=VALUE ...] PROGRAM ARGUMENT...
```

일반 실행에서는 `bash -c`를 사용하지 않는다. Make/Tcl pipeline처럼 shell이
필요한 backend만 명시적으로 `/bin/bash -lc`를 사용하며 shell script 생성과
escaping 책임은 해당 adapter에 둔다.

프로세스 실행 원칙:

- Win32 message loop를 차단하지 않는다.
- 논리 CPU 수를 기준으로 bounded worker와 CPU token budget을 사용한다.
- 여러 Design++ 프로세스가 하나의 cross-process CPU quota를 공유한다.
- stdout/stderr를 worker thread에서 실시간 수집한다.
- UI 변경은 window message를 통해 GUI thread에서 수행한다.
- Windows Job Object로 `wsl.exe` 하위 프로세스 전체를 취소한다.
- 프로세스 exit code와 Design++ stage 성공 여부를 구분한다.
- raw log를 보존하고 정규화된 diagnostics/metrics를 별도로 저장한다.

## 8. 프로젝트 저장 구조

Cell별 프로젝트 저장 구조는 다음과 같다.

```text
<library>/cells/<cell-uuid>/
  project.dpproj
  views/
  .designpp/
    project.writer.lease
    staging/
    recovery/
    runs/
    <run-id>/
      manifest.json
      events.jsonl
      logs/
      reports/
      artifacts/
      metrics.json
```

`.dpproj`에는 schema version과 Library/Cell UUID를 기록한다. 관리 파일은
`.dplib`가 소유하고 `.dpproj`는 파일별 override만 저장한다. 자세한 형식은
[PROJECT_FORMAT.md](PROJECT_FORMAT.md)를 참고한다.

초기 버전에서는 별도 데이터베이스를 도입하지 않고 versioned project file과
run별 manifest를 사용한다. 프로젝트가 커져 검색 성능 문제가 생길 때 SQLite
도입을 별도로 검토한다.

## 9. GUI 계획

```text
┌ Project Explorer ┬──────── Flow / Result View ────────┬ Inspector ┐
│ RTL               │ 선택 Stage 설정과 결과             │ Inputs    │
│ Constraints       │ 리포트, 메트릭, 물리 설계 결과      │ Options   │
│ Testbench         │                                   │ Outputs   │
│ Runs              │                                   │           │
├───────────────────┴───────────────────────────────────┴───────────┤
│ Problems | Log | Metrics | Command Preview | Run History         │
└───────────────────────────────────────────────────────────────────┘
```

### 메뉴 매핑

| Design++ 메뉴 | 대응하는 상용 도구 개념 | 실제 오픈소스 backend |
|---|---|---|
| RTL Simulation | VCS | Verilator/Icarus/cocotb |
| Waveform Debug | Verdi | GTKWave |
| Synthesis | Design Compiler | Yosys |
| Timing Analysis | PrimeTime | OpenSTA |
| Physical Design | ICC2/Fusion Compiler | OpenROAD |
| Layout Viewer | Custom Compiler Viewer | KLayout/OpenROAD GUI |
| DRC/LVS | IC Validator | Magic/Netgen |
| Full Flow | DC → ICC2 → PrimeTime | OpenLane 2/ORFS |

Synopsys 제품은 용어와 작업 흐름의 참고 대상으로만 사용한다. 아이콘, 자산,
화면 배치나 proprietary database를 그대로 복제하지 않는다.

## 10. Synopsys 프로젝트 가져오기

### 우선 지원 형식

- `.v`, `.sv`, `.vh`
- file list, include, define 옵션
- SDC
- Liberty
- LEF/DEF
- SPEF/SDF
- SAIF
- VCD/FST
- GDSII

### 직접 지원하지 않는 형식

- FSDB
- DDC
- Milkyway database
- ICC2/Fusion Compiler 내부 database
- NDM 등 proprietary format

이 형식들은 사용자가 portable format으로 export하거나 정식 vendor API/변환
도구를 사용할 수 있을 때만 연결한다.

Import Wizard 단계:

1. 소스와 file list 분석
2. top module 후보 탐색
3. include/define/library 매핑
4. SDC 명령 호환성 검사
5. PDK/library 연결
6. 미지원 옵션과 파일 형식 보고
7. `.dpproj` 생성

## 11. 단계별 개발 로드맵

### Phase 0 — Win32/WSL2 기반 구축

상태: 완료

- Visual Studio/MSBuild 프로젝트
- 순수 Win32 Library Manager 시작 메인 창
- 좌측 탐색기/중앙 목록/하단 Output 목업 레이아웃만 유지
- 프로젝트·Flow 목업 데이터 제거, 실제 라이브러리 연결 전 빈 목록 표시
- 표준 Win32 파일/도구/도움말 메뉴와 독립 Tool Check 창
- 도구 검사·설정 작업의 메인 창 통합 로그
- 기본 RTL-to-GDS stage 모델
- 비동기 Win32 process runner
- Job Object 기반 process-tree 취소
- 구조화된 WSL2 command builder
- WSL2 연결 진단
- x64 Debug 빌드 및 실제 WSL2 실행 검증

### Phase 1 — Toolchain 진단 및 실행 리소스 관리

상태: 완료

- [완료] WSL 배포판 목록과 기본 배포판 탐색
- [완료] WSL1/WSL2 구분
- [완료] CPU token 기반 bounded task scheduler
- [완료] 외부 EDA 도구의 CPU 사용량을 포함하는 resource accounting
- [완료] 여러 Design++ 프로세스가 공유하는 named-semaphore CPU quota
- [완료] 별도 Windows 자식 프로세스가 quota를 점유하는 process-boundary 회귀
  검증
- [완료] 취소, shutdown, 순서 보장과 high-task-count stress test
- [완료] tool executable/version probe
- [완료] WSL2/Ubuntu 초기 설정과 설치 단계 전용 권한 상승
- [완료] APT/cocotb/Nix/OpenLane 2/ORFS 설치·업데이트 pipeline
- [완료] Tool Check 선택 도구 개별 설치·삭제와 managed dependency 보호
- [완료] Windows WebView2 Runtime 탐지와 선택 설치·복구
- [완료] 실시간 설정 로그, 진행률, 취소 UI
- [완료] Library Manager/Tool Check Per-Monitor V2 high-DPI 대응
- [완료] OpenLane 2, ORFS 설치 경로 설정
- [완료] PDK root 설정과 유효성 검사
- [완료] Toolchain Profile CRUD와 저장
- [완료] 독립 GUI Doctor 화면

완료 기준: 사용자가 프로젝트 생성 전에 실행 가능한 도구와 누락된 도구를
한 화면에서 확인할 수 있고, 동시에 여러 작업이나 GUI를 실행해도 설정된
CPU budget을 초과하지 않는다.

### Phase 2 — Library 저장 및 프로젝트 작업 공간

상태: 완료

- [완료] 공용 Library Root 설정과 자동 탐색
- [완료] `Library → Cell → View` 도메인과 `.dplib` schema v1
- [완료] Library/Cell/View 생성, 속성 수정, 관리 파일 가져오기와 삭제
- [완료] atomic manifest 저장, revision 충돌 검사 및 writer lease
- [완료] 로컬 휴지통 삭제와 UNC 영구 삭제 확인 경계
- [완료] 비동기 Tree/List 갱신과 Workspace open request 연결
- [완료] Library/Cell/View 3열 브라우저와 영역별 검색·상태/종류 필터
- [완료] 검색 exact-match 이동과 미존재 이름의 인라인 생성 요청
- [완료] `ViewWindow`/factory 기반 top-level 도구 창 수명 관리와 동일 Cell
  Verilog 창 재사용
- [완료] `.dpproj` schema v2, v1 migration 및 첫 열기 자동 생성
- [완료] 명시적 Save/Ctrl+S와 dirty 종료 확인
- [완료] `.dplib` 기반 Source Set 자동 동기화와 파일별 제외
- [완료] top module, include, define, parameter, constraint, CPU 설정
- [완료] Source Set 기반 top module, parameter, include, constraint 기본값 추론
- [완료] 단일 top module RTL 저장 시 module 이름 기반 파일명 자동 정규화
- [완료] Windows 경로와 WSL 경로 매핑
- [완료] `.designpp/runs` 생성과 run manifest
- [완료] 창별 독립 project context와 session generation
- [완료] 동일 프로젝트의 cross-process writer lease와 read-only fallback
- [완료] UUID 기반 독립 run directory와 atomic project save
- [완료] Registry 기반 최근 Workspace 목록, 중복 제거, stale entry 정리와
  Library Manager 재열기
- [완료] 독립 store 동시 read/merge/write에서 최근 Workspace 유실 방지 검증

완료 기준: 앱을 재시작해도 Library와 프로젝트 설정이 동일하게 복원되고,
여러 창과 여러 프로세스가 같은 Library/프로젝트를 열어도 설정이나 run이
손상되지 않는다.

### Phase 3 — Verilator Lint vertical slice

상태: 첫 vertical slice 완료

- [완료] capability 기반 `ToolAdapter` 경계
- [완료] Verilator probe, 검증과 구조화 command generation
- [완료] Lint configuration UI
- [완료] 실시간 raw log 보존과 Library Manager 미러링
- [완료] warning/error parser
- [완료] Problems와 Runs pane
- [완료] 실행 취소와 재실행
- [완료] cross-process CPU quota와 Interrupted run 복구

완료 기준: 예제 RTL을 열고 GUI에서 Lint를 실행한 뒤 오류 파일과 줄 번호를
확인할 수 있다.

### Phase 3.5 — Monaco HDL 편집기

상태: v1 구현 완료

- [완료] WebView2 기반 로컬 Monaco Editor 내장
- [완료] `.v`, `.vh`, `.sv`, `.svh` 다중 model/tab 편집
- [완료] Verilog/SystemVerilog Monarch syntax highlighting
- [완료] undo/redo, find/replace, go-to-line과 model별 view state
- [완료] 명시적 Save, 직렬 Save All과 dirty 종료 확인
- [완료] UTF-8/BOM/EOL 보존과 16 MiB 편집 한계
- [완료] content hash/revision/writer lease 기반 충돌 차단
- [완료] Workspace 단위 재귀 file watcher와 외부 변경 알림
- [완료] Verilator marker와 Problems 위치 이동
- [완료] local virtual host, CSP와 versioned JSON protocol
- [완료] WebView2 Runtime 실패 시 Workspace 기능 유지
- [완료] 앱 COM STA 초기화와 WebView2 실패 HRESULT 진단

후속 범위는 HDL LSP/IntelliSense, symbol navigation, formatting provider,
Git, terminal, Monaco source breakpoint 및 VS Code extension compatibility다.

### Phase 4 — RTL Simulation

상태: 완료

- [완료] Icarus Testbench 실행 (`iverilog` → `vvp`)
- [완료] 활성 Testbench 탭 기반 실행 Inspector
- [완료] 파일별 Testbench Top/VCD 설정과 `.dpproj` schema v2
- [완료] VCD artifact 등록과 GTKWave 수동 실행
- [완료] VVP 대화형 디버거의 시작 정지, Continue, event Step, Finish
- [완료] Debug scope/variable console과 `$stop` Monaco 위치 이동
- [완료] bounded interactive stdin과 Debug run/VCD 기록
- [완료] Run/Debug execution provider와 generation-safe terminal delivery
- [완료] 별도 WSL Icarus/VCD/WSLg/cancellation integration test gate
- [완료] Verilator simulation과 Workspace 실행 연결
- [완료] cocotb runner와 Icarus/Verilator backend 선택
- [완료] test configuration 관리 (`.dpproj` schema v3와 Inspector UI)
- [완료] VCD/FST 생성 선택, artifact 등록과 재시작 복원
- [완료] GTKWave 실행 상태와 오류 출력
- [완료] WSLg `[WARN:COPYMODE]` 공유 메모리 장애 사전 진단 및 선택 배포판
  자동 복구
- [완료] process/test 결과 분리, xUnit 원본 보존, schema v2 summary와
  testcase별 Tests 화면 및 과거 Run 복원
- [완료] managed cocotb venv probe, cocotb 1.9/2.x invocation 호환, 실제
  Icarus/VCD 및 Verilator/FST integration gate

대화형 디버거 v1은 `vvp -i -s`와 Testbench의 `$stop`을 사용한다. 일반 Run은
`vvp -N`으로 `$stop`을 실패 종료로 처리한다. Monaco breakpoint, 실행 중 임의
Pause, statement 단위 stepping과 persistent Watch는 후속 범위다.

완료 기준: GUI에서 테스트를 실행하고 생성된 waveform을 GTKWave로 열 수
있다.

### Phase 5 — Standalone Synthesis/STA

상태: Yosys/Schematic/OpenSTA vertical slice 및 안정화 완료

- [완료] Yosys adapter와 비동기 `SynthesisRunService` (probe, enabled RTL/top
  검증, CPU quota, script, Run/Cancel, artifact/statistics 판정, exactly-once
  completion) 및 전용 `SynthesisWindow` 연결
- [완료] Monaco 기반 `VerilogWindow`와 비-Monaco `SynthesisWindow` top-level
  클래스 분리, 공통 `ViewWindowFactory` 라우팅
- [완료] synthesis script, JSON/Verilog netlist, statistics/report와 summary
  artifact 보존 및 상단 Script/Reports/Artifacts 표시
- [완료] 공백/한글 Library 경로를 위한 run UUID별 Yosys ASCII-safe staging과
  Windows-side artifact 보존
- [완료] cell count/area parser와 전용 Reports 표시
- [완료] ABC 이전 `schematic-structural.json`과 ABC 이후 `netlist.json`을
  분리 보존하고, 기본 Readable inferred-logic 보기와 실제 Gate 보기를 전환
- [완료] tool-neutral `SchematicModel`과 worker 기반
  `SchematicBuildService`(normalize, validate, sequential-boundary depth, SCC,
  layout, orthogonal routing)를 도입하고 GDI canvas는 immutable scene만 렌더링
- [완료] register/register-bank, arithmetic, comparator, mux, generic block,
  bus 폭 표기, clock/reset/enable 역할·polarity 및 feedback lane 표현
- [완료] IEEE distinctive-shape primitive, obstacle 회피, shared trunk,
  non-connecting bridge, red pin/blue branch 의미 색상, 확대·이동·Fit 유지
- [완료] Gate 보기가 2,000 nodes 또는 8,000 connections를 넘으면 부분 회로를
  표시하지 않고 명시적 크기 진단 제공
- [완료] Yosys 성공·실패·취소·artifact 계약, RTL fixture matrix, 대형
  Schematic fast-router, Canvas Fit/LOD/GDI 누수·렌더링 예산 회귀 테스트
- [완료] OpenLane 2 Nix 환경의 OpenSTA adapter와 비동기
  `TimingRunService` (probe, 합성 호환성 검증, CPU quota, ASCII-safe staging,
  setup/hold 실행, 취소 및 exactly-once completion)
- [완료] 독립 `TimingWindow`, 관리 SDC/Liberty와 corner 설정 저장,
  Synthesis 이동, Reports/Artifacts/Script 표시
- [완료] Library manifest schema v3 shared managed files와 Library-level
  Liberty 가져오기/표시 및 모든 Cell의 Synthesis/Timing 후보 공유
- [완료] setup/hold WNS/TNS, path 존재 여부와 최대 1,000개 violation parser,
  process success와 timing result success의 분리 기록
- [완료] synthesis summary schema v2 configuration/Liberty fingerprint와 최신
  호환 mapped netlist 선택 계약
- [완료] Timing 결과의 `ns` 단위 명시, setup/hold/recovery/removal 분리 집계,
  표 기반 violation 표시와 timing summary schema v2 저장
- [완료] OpenSTA 진단과 Nix 환경의 원시 경고를 분리하고, 원시 출력은 run log에
  그대로 보존
- [완료] Timing Run history의 corner/WNS/TNS 표와 과거 Run 선택 복원
- [완료] Timing service의 nonzero/missing artifact/malformed report/staging
  failure/stale generation/shutdown 계약 테스트 행렬
- [완료] 숨김 `TimingWindow` 생성·상태·message responsiveness smoke와 한글·공백
  Library 입력의 OpenSTA ASCII-safe staging 회귀 테스트

완료 기준: RTL에서 netlist를 생성하고 timing 결과를 GUI에서 비교할 수 있다.

### Phase 6 — Backend Physical Implementation + Layout View

- [완료] Library schema v5에서 legacy Physical Design View를 Layout View로
  메모리 migration하고 새 Physical Design View 생성을 제거
- [완료] Project schema v10의 tool-neutral
  `PhysicalImplementationConfiguration`, v5 `openlane` 설정 migration 및
  PDK-default/사용자 override를 구분하는 PDN grid 설정
- [완료] OpenLane JSON config 생성과 managed RTL/include/define/parameter/SDC
  staging
- [완료] `ManagedFlowAdapter`/`ManagedFlowRunService` 확장 경계와 OpenLane 2
  Classic full flow 실행
- [완료] exact OpenLane step ID의 Design++ stage 정규화와 단계별 진행 로그
- [완료] METRICS2.1 known metric 정규화 및 unknown raw metric 보존
- [완료] immutable state, final view, report, raw log와 checkpoint artifact 수집
- [완료] Magic DRC, Netgen LVS, KLayout DRC/XOR 원문 리포트를 backend-relative
  경로와 파일명 그대로 Run의 `reports/openlane`에 보존하고 artifact로 색인
- [완료] fingerprint/lineage/checkpoint 기반 실패 Run resume 계약
- [완료] `PhysicalImplementationService`의 compatible GDS 재사용, stale 입력
  재실행 및 compatible checkpoint 자동 resume 계약
- [완료] Layout 재시작 시 고아 Running/Queued Run을 Interrupted로 복구하고
  compatible checkpoint를 자동 resume 후보로 유지
- [완료] 독립 `LayoutWindow`의 Generate/Cancel/Setup/Open Layout/Reports/
  Artifacts UX와 외부 KLayout WSLg 연동
- [완료] Layout Setup의 multilayer/core-ring/rail 및 수직·수평
  width/spacing/pitch/offset PDN 설정과 OpenLane config/fingerprint 연결
- [완료] absolute floorplan의 Die area와 내부 Core area 직접 설정·검증
- [완료] Tap/endcap insertion의 `FP_TAPCELL_DIST`를 µm 단위 선택값으로
  Layout Setup, persistence, config 및 fingerprint에 연결
- [완료] DPI 대응 탭형 Layout Setup과 matching/random/annealing I/O 자동 배치,
  중첩 N/S/E/W 방향 탭 기반 사용자 pin-order, PDK-default 안내, run별
  `pin_order.cfg` 및 checkpoint fingerprint 연결
- [완료] 폼 설정과 안전한 unknown override를 양방향 동기화하는 별도 OpenLane
  Monaco JSON Validate/Format/Apply 편집기와 managed/path/nested/duplicate key
  거부, 요청 시점 비동기 텍스트 snapshot, Save Setup/Ctrl+S의 원자적 프로젝트
  저장 연결. resolved config의 run-owned `FP_PIN_ORDER_CFG` 경로는 읽거나
  저장하지 않고 무시하며 나머지 typed 설정은 적용
- [완료] KLayout 실행 전 WSLg shared-memory health check, root-owned 0755
  디렉터리의 1777 권한 복구, 안전한 tmpfs fallback을 공통 Viewer 실행 경계에
  연결
- [완료] OpenLane 2 Classic은 사용자-facing View/Window가 아닌 managed
  RTL-to-GDS backend로만 등록
- [완료] 실제 OpenLane 2.3.10/sky130A에서 한글·공백 staging, Floorplan
  checkpoint 생성, exact step resume 및 tiny sequential RTL-to-GDS final artifact
  WSL integration fixture

완료 기준: 공개 PDK와 예제 설계로 RTL-to-GDS 전체 실행을 완료하고 Layout
View에서 compatible GDS 상태를 확인하고 KLayout으로 열 수 있다.

### Phase 7 — ORFS 단계 실행·Checkpoint·OpenROAD GUI

- [완료] 외부 `DESIGN_CONFIG`(`config.mk`) 생성과 Unicode/공백 입력 staging
- [완료] ORFS target과 Design++ Stage 매핑 및 backend capability registry
- [완료] synthesis/floorplan/place/CTS/route/finish 단계 실행 경계
- [완료] ORFS Make artifact graph 직렬화와 `NUM_CORES` 기반 내부 도구 병렬화
- [완료] checkpoint, compatible target 재사용, fingerprint 기반 resume 경계
- [완료] 실제 RTL/include/SDC 내용과 활성 ORFS 설정을 먼저 고정하는
  `PreparedPhysicalInputs` 계약. 자동 resume의 불일치는 실패 대신 새 lineage로
  전환하고, 명시적 Rebuild From 불일치는 원인과 Run ID를 보존하며 거부
- [완료] managed SDC의 명시적 시간 단위를 존중하는 clock 정규화와 실제
  최단 clock 기반 `CLOCK_PERIOD`/ABC 제약 생성. raw metric은 보존하고
  단위를 확인한 Layout timing 값만 ns로 표시
- [완료] ORFS `write_sdc`가 command unit을 기록하지 않는 동작을 Run별
  OpenROAD 초기화 계약으로 보완. 합성 이후 새 OpenROAD 프로세스와
  resume/viewer에서도 managed SDC의 최종 명시 단위를 다시 적용하며, 이
  계약 변경은 기존 checkpoint를 stale 처리
- [완료] ORFS platform discovery와 incomplete platform 진단
- [완료] checkout·Make·OpenROAD executable을 함께 검사하는 capability probe와
  lineage별 cross-process exclusive writer lease
- [완료] ORFS flake의 오래된 Yosys 고정값 대신 checkout의 `tools/yosys` 및 ABC를
  probe와 실행에 동일하게 연결하고 `stat -hierarchy`/`read_lib -m`을 검사한다.
  ABC 전략 강제 변경이나 Liberty·합성 스크립트 변환은 사용하지 않는다.
- [완료] ASAP7 압축 multi-Liberty 합성 integration: 한글 원본의 ASCII staging,
  실제 adapter probe/실행, netlist 및 `1_synth.odb` 생성 검증.
- [완료] OpenROAD checkpoint viewer의 독립 WSLg 실행 경계
- [완료] 반복 metric 보존과 공통 area/utilization/timing/congestion raw map 경계
- [완료] sky130hd/nangate45 공개 GCD fixture의 full flow, 최종 artifact 및
  OpenROAD headless ODB load. sky130hd post-CTS EQY 검사도 활성 상태로 통과.

완료 기준: ORFS를 포크하지 않고 외부 프로젝트 설정으로 전체 또는 선택
단계를 실행할 수 있고, Layout Stages에서 checkpoint와 OpenROAD viewer를
호출할 수 있다.

### Phase 8 — 물리 검증 및 Layout 확장

- [구현] ToolchainSettings v3의 활성·롤백 환경 참조, 설치 환경 inventory와
  사용자/managed verification recipe registry
- [구현] Run manifest v4의 source Run, 실제 environment ID와 fingerprint
- [구현] Project schema v12의 Cell별 physical verification 설정과 무변경
  migration
- [구현] `PhysicalVerificationService`의 별도 Run, CPU quota, probe/execute,
  cancellation, raw log, input hash manifest와 원본 report 보존
- [구현] KLayout DRC/LVS, Magic DRC, Netgen LVS adapter 및 빈/malformed 결과의
  PASS 방지
- [구현] Layout의 Summary/Verification/Runs 탭, DRC/LVS/전체 검증, 현재 GDS와
  다른 source Run 결과의 Stale 표시
- [진행] DEF/LEF 및 marker 선택을 KLayout 위치 탐색으로 연결
- [진행] OpenLane sky130A Magic 추출+Netgen 기본 recipe와 사용자 recipe 신뢰
  등록 UI
- [검증 필요] 현재 Timer/ASAP7 및 sky130hd/nangate45 실제 규칙 회귀. ASAP7
  LVS는 호환 recipe 등록 전 비활성 상태를 유지한다.

완료 기준: 최종 GDS를 열고 DRC/LVS pass/fail과 원본 report 및 상세 오류를
확인할 수 있으며, 실제 환경 회귀와 Debug/Release gate가 모두 통과해야 한다.

### Phase 9 — Import와 안정화

- Synopsys portable project import wizard
- app/run crash recovery
- 설정 migration
- 로그 검색과 export
- run 비교와 QoR comparison
- Release build와 배포 패키지
- 사용자 문서 및 예제 프로젝트

## 12. 릴리스 목표

| 버전 | 목표 |
|---|---|
| v0.1 | 현재 Win32/WSL2 foundation |
| v0.2 | 프로젝트 저장과 Toolchain Doctor |
| v0.3 | Verilator Lint |
| v0.35 | Monaco HDL 편집기와 진단 연결 |
| v0.4 | RTL Simulation, cocotb, GTKWave |
| v0.5 | Yosys와 OpenSTA |
| v0.7 | OpenLane 2 Full Flow |
| v0.8 | ORFS 단계별 Physical Design |
| v0.9 | KLayout, Magic, Netgen, Import Wizard |
| v1.0 | 복구, run 비교, 문서, 배포 안정화 |

## 13. 테스트 전략

### Unit Test

- Project validation
- Flow graph와 dependency validation
- command argument 생성 및 escaping
- Windows/WSL path mapping
- 설정 hash와 stale 판정
- log parser

### Contract Test

실제 EDA 도구 대신 fake executable을 사용한다.

- 정상 종료
- 비정상 exit code
- stdout/stderr 스트리밍
- 긴 실행 취소
- 하위 프로세스 정리
- artifact 미생성

### Integration Test

- 작은 counter/GCD RTL lint
- Verilator/Icarus simulation
- Yosys synthesis
- OpenSTA timing
- 공개 PDK 기반 소형 RTL-to-GDS smoke test

### GUI Test

- 프로젝트 생성과 복원
- 실행 중 UI 응답성
- Problems 항목 선택
- run history 복원
- 실행 취소
- WSL/EDA tool 미설치 상태의 오류 안내

일반 빌드에서는 빠른 unit/contract test를 실행하고, 무거운 전체
RTL-to-GDS 검증은 nightly 또는 수동 테스트로 분리한다.

## 14. 저장할 핵심 메트릭

| 단계 | 메트릭 |
|---|---|
| Lint | error/warning 개수와 종류 |
| Simulation | test pass/fail, 실행 시간 |
| Synthesis | cell count, area, memory, runtime |
| STA | WNS, TNS, violating path count |
| Floorplan | die/core area, utilization |
| Placement | density, congestion, timing |
| CTS | skew, latency, buffer count |
| Routing | wire length, vias, congestion, timing |
| DRC/LVS | violation count, match status |
| Final | GDS 경로/크기, 전체 runtime, peak memory |

adapter가 생성하는 원본 metric은 보존하면서 GUI가 사용하는 공통 metric
schema로 정규화한다.

## 15. 주요 위험과 대응

| 위험 | 대응 |
|---|---|
| 도구 버전별 CLI/로그 차이 | adapter별 version probe와 parser fixture |
| WSL/Windows 경로 불일치 | 중앙 PathMapper와 round-trip test |
| PDK 라이선스와 설치 위치 | PDK를 저장소에 포함하지 않고 profile로 참조 |
| `latest` 이미지에 의한 결과 변화 | 이미지 digest와 도구 버전을 manifest에 기록 |
| 로그 parser 취약성 | raw log 보존, 구조화 출력 우선, golden test |
| 장기 실행 중 UI 정지 | 모든 실행 비동기 처리와 window message marshal |
| 취소 후 고아 프로세스 | Job Object와 WSL process-tree 종료 테스트 |
| 중간 artifact 오용 | PDK/corner/input hash compatibility 검사 |
| proprietary format 요구 | portable format import 우선, 미지원 명확히 보고 |

## 16. 개발 원칙

- Win32 window procedure에서 EDA 명령을 직접 만들지 않는다.
- GUI에서 가능한 실행은 추후 headless runner에서도 재현 가능하게 설계한다.
- 명령은 shell 문자열이 아니라 executable과 argument 배열로 저장한다.
- raw log와 생성 artifact를 삭제하지 않고 run directory에 보존한다.
- public 함수에는 Doxygen 주석을 작성한다.
- RAII를 따르고 owning raw `new`/`delete`를 사용하지 않는다.
- 외부 라이브러리는 사전 승인 없이 추가하지 않는다.
- OpenLane 2와 ORFS의 내부 구현을 복제하지 않는다.
- 자체 waveform/layout viewer 및 P&R 알고리즘은 v1 범위에 포함하지 않는다.
- Debug x64 빌드가 성공한 상태로 변경을 마무리한다.

## 17. 다음 구현 순서

현재 foundation 이후 작업 순서는 다음으로 확정한다.

1. Google C++ Style Guide 기반 기존 scaffold 정규화
2. CPU token scheduler와 cross-process resource coordinator
3. WSL 배포판 및 설치된 EDA 도구 탐색
4. Toolchain Profile과 Doctor 화면
5. `.dpproj` schema, writer lease 및 New/Open/Save
6. Tool Adapter 인터페이스
7. Verilator Lint vertical slice
8. Problems pane와 source navigation
9. RTL simulation/cocotb/GTKWave
10. Yosys/OpenSTA
11. OpenLane 2
12. ORFS와 Physical Design

첫 실제 EDA 기능은 Verilator Lint로 한다. 이 단계에서 프로젝트 저장,
WSL 실행, 로그 스트리밍, parser, Problems UI, 취소, run 기록까지 한 번에
연결하여 이후 adapter가 같은 구조를 재사용하게 한다.
