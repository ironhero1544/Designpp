# Design++ 제작 계획

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
| 프로젝트 파일 | 버전이 있는 `.dpproj` 형식 예정 |
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

예정된 사용자 프로젝트 구조는 다음과 같다.

```text
project.dpproj
rtl/
tb/
constraints/
scripts/
.designpp/
  runs/
    <run-id>/
      manifest.json
      events.jsonl
      logs/
      reports/
      artifacts/
      metrics.json
```

`.dpproj`에는 schema version을 반드시 기록한다. 사용자 RTL과 생성물을
분리하고 `.designpp/runs` 아래 run directory는 실행 중에도 순차적으로
기록한다.

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

상태: 진행 중 (Library Manager 기반 기능 구현)

- WSL 배포판 목록과 기본 배포판 탐색
- WSL1/WSL2 구분
- CPU token 기반 task scheduler
- 외부 EDA 도구의 CPU 사용량을 포함하는 resource accounting
- 여러 Design++ 프로세스가 공유하는 cross-process CPU quota
- 취소, shutdown, fairness stress test
- [완료] tool executable/version probe
- [완료] WSL2/Ubuntu 초기 설정과 설치 단계 전용 권한 상승
- [완료] APT/cocotb/Nix/OpenLane 2/ORFS 설치·업데이트 pipeline
- [완료] Tool Check 선택 도구 개별 설치·삭제와 managed dependency 보호
- [완료] 실시간 설정 로그, 진행률, 취소 UI
- [완료] Library Manager/Tool Check Per-Monitor V2 high-DPI 대응
- OpenLane 2, ORFS 설치 경로 설정
- PDK root 탐색과 유효성 검사
- Toolchain Profile 저장
- GUI Doctor 화면

완료 기준: 사용자가 프로젝트 생성 전에 실행 가능한 도구와 누락된 도구를
한 화면에서 확인할 수 있고, 동시에 여러 작업이나 GUI를 실행해도 설정된
CPU budget을 초과하지 않는다.

### Phase 2 — 프로젝트 저장 및 작업 공간

- `.dpproj` schema v1
- New/Open/Save/Save As
- RTL source 추가와 제거
- top module, include, define 설정
- Windows 경로와 WSL 경로 매핑
- `.designpp/runs` 생성과 run manifest
- 창별 독립 project context
- 동일 프로젝트의 cross-process writer lease와 read-only fallback
- UUID 기반 독립 run directory와 atomic project save
- 최근 프로젝트 목록

완료 기준: 앱을 재시작해도 프로젝트와 설정이 동일하게 복원되고, 여러 창과
여러 프로세스가 같은 프로젝트를 열어도 설정이나 run이 손상되지 않는다.

### Phase 3 — Verilator Lint vertical slice

- `IToolAdapter`/adapter registry
- Verilator probe와 command generation
- Lint configuration UI
- 실시간 로그
- warning/error parser
- Problems pane와 source 위치 이동
- 실행 취소와 재실행

완료 기준: 예제 RTL을 열고 GUI에서 Lint를 실행한 뒤 오류 파일과 줄 번호를
확인할 수 있다.

### Phase 4 — RTL Simulation

- Verilator simulation
- Icarus 선택 backend
- cocotb runner
- test configuration 관리
- VCD/FST artifact 등록
- GTKWave 실행
- simulation 성공/실패와 test summary

완료 기준: GUI에서 테스트를 실행하고 생성된 waveform을 GTKWave로 열 수
있다.

### Phase 5 — Standalone Synthesis/STA

- Yosys adapter
- synthesis script 생성과 command preview
- netlist/report artifact
- cell count/area parser
- OpenSTA adapter
- SDC/Liberty corner 설정
- WNS/TNS 및 violation browser

완료 기준: RTL에서 netlist를 생성하고 timing 결과를 GUI에서 비교할 수 있다.

### Phase 6 — OpenLane 2 Managed Flow

- OpenLane project/config 생성
- full flow 실행
- Step/State/Metric 매핑
- 단계별 진행률과 로그 분리
- artifact와 metrics 수집
- 실패 단계 재실행/resume
- GDSII와 최종 report 등록

완료 기준: 공개 PDK와 예제 설계로 RTL-to-GDS 전체 실행을 완료하고 결과를
Design++에서 탐색할 수 있다.

### Phase 7 — ORFS Managed Flow

- 외부 `DESIGN_CONFIG` 생성
- ORFS stage와 Design++ Stage 매핑
- synthesis/floorplan/place/CTS/route 단계 실행
- checkpoint와 resume
- OpenROAD GUI 연동
- congestion, utilization, clock, timing metrics

완료 기준: ORFS를 포크하지 않고 외부 프로젝트 설정으로 전체 또는 선택
단계를 실행할 수 있다.

### Phase 8 — Layout 및 DRC/LVS

- KLayout 실행 및 GDS/LEF/DEF 열기
- Magic DRC adapter
- Netgen LVS adapter
- DRC marker와 LVS mismatch parser
- physical verification 결과 화면

완료 기준: 최종 GDS를 열고 DRC/LVS pass/fail과 상세 오류를 확인할 수 있다.

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
