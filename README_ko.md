# Design++

![Design++ — Windows 네이티브 반도체 통합 설계 환경](docs/images/main.png)

> **Semiconductor IDE (Integrated Design Environment)**
> 반도체 통합 설계 환경

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)](https://isocpp.org/)
![Platform](https://img.shields.io/badge/Platform-Windows%20x64-0078D4)
![UI](https://img.shields.io/badge/UI-Native%20Win32-0078D4)
![EDA](https://img.shields.io/badge/EDA-WSL2-orange)

[English](README.md) | **한국어**

**Design++** 는 Windows에서 반도체 설계 작업을 수행하기 위한 통합 설계 환경입니다.
HDL 편집, 시뮬레이션, 논리 합성, 타이밍 분석, 물리 설계와 검증을
Library / Cell / View 기반의 작업 공간으로 연결합니다.

현재 구현은 디지털 회로의 RTL-to-layout 설계에 초점을 맞추고 있습니다.
소스와 제약을 구성하고, PDK와 도구 환경을 선택하고, 설계 단계를 실행하고,
결과를 검토하는 작업을 같은 환경에서 수행합니다. 데스크톱 UI는 Windows에서
네이티브로 동작하며 Linux EDA 도구는 WSL2에서 실행합니다.

## 프로젝트의 목적

반도체 설계에서는 하나의 회로를 RTL, 테스트벤치, 합성된 netlist, 타이밍 제약,
물리 Layout 등 여러 형태로 다룹니다. 각 단계에는 서로 다른 도구와 설정,
로그 및 생성 파일이 필요합니다.

Design++는 이러한 작업을 설계 단위로 연결하는 것을 목표로 합니다.

- Library, Cell, View로 설계를 구성하고 관리합니다.
- 소스 편집에서 시뮬레이션, 합성, 물리 설계로 작업을 이어갑니다.
- 도구 환경과 PDK 설정을 설계 작업 안에서 관리합니다.
- 소스와 연결된 진단, 보고서 및 이전 실행 결과를 확인합니다.

## 주요 기능

### Library Manager와 설계 작업 공간

**Library → Cell → View** 구조로 프로젝트를 탐색합니다. Cell은 설계의 소스와
설정을 묶고, View는 RTL·테스트벤치·제약·합성·타이밍·Layout 작업을 구분합니다.
여러 Cell을 독립된 작업 공간에서 열 수 있으며 중앙 Output에서 작업별 로그를 확인합니다.

![Test Library와 Timer Cell 및 View를 보여주는 Library Manager](docs/images/library-manager.jpg)

### HDL 편집과 시뮬레이션

내장 **Monaco Editor**에서 Verilog/SystemVerilog를 편집하고 진단 위치로 이동합니다.
Top module, include 경로, define과 parameter를 설정할 수 있습니다.
Verilator lint, Icarus/Verilator 시뮬레이션, cocotb 실행과 GTKWave 연동으로
회로 및 테스트벤치의 동작을 확인합니다.

![소스 트리와 Monaco HDL 편집기가 열린 Verilog 작업 창](docs/images/hdl-editor.jpg)

### 합성과 타이밍 분석

**Yosys**로 RTL을 선택한 셀 라이브러리에 매핑하고 netlist와 합성 보고서를 확인합니다.
**OpenSTA**는 호환되는 합성 결과, Liberty 라이브러리와 타이밍 제약을 사용합니다.
생성된 artifact와 원본 출력은 Run 기록에 보존합니다.

### 물리 설계와 검증

**OpenLane 2**와 **OpenROAD Flow Scripts (ORFS)**로 물리 설계를 실행합니다.
ORFS는 단계별 실행과 호환되는 checkpoint 재개를 지원합니다. Layout 작업 공간에서
실행 이력, 메트릭, 생성물과 검증 결과를 확인하고 외부 viewer를 연결합니다.

![합성부터 Final까지 완료되고 checkpoint를 사용할 수 있는 ORFS 단계 창](docs/images/physical-flow-stages.jpg)

PDK 선택은 Cell별로 저장합니다. 독립 DRC/LVS 지원은 backend와 등록된 recipe에
따라 달라집니다. ORFS sky130hd에는 DRC/LVS recipe가 있으며 ASAP7은 DRC만
지원합니다. OpenLane의 검증 결과는 flow 보고서에서 수집합니다.

### 도구 환경 관리

**Tool Check**에서 환경 준비, 검사, 활성화와 롤백을 수행합니다.
**Toolchain Doctor**에서는 WSL 배포판과 사용자 지정 경로를 관리합니다.
실행 전에 framework revision과 필수 도구 기능을 검사합니다.

![설치된 EDA 도구 버전을 확인한 Tool Check 창](docs/images/tool-check.jpg)

화면의 버전은 로컬 예제 PC에서 감지한 값입니다. 이 버전은 Tool Check에서
관리하는 OpenLane 2 2.3.10과 ORFS 26Q2를 지원하며, 실행 전 실제 revision과
필수 도구 기능도 확인합니다.

긴 작업은 UI 밖에서 실행하고 진행 상황, 취소와 CPU 자원 조정을 지원합니다.
원자적 저장과 writer lease로 여러 창이나 프로세스의 저장 충돌을 방지합니다.

## 설계 흐름

```text
Library / Cell / View
        ↓
RTL + Testbench + Constraints
        ├── Lint / Simulation → 진단 / 파형
        ↓
Synthesis → Netlist → Timing Analysis
        ↓
Physical Implementation → Layout / GDS
        ↓
지원되는 DRC / LVS → 보고서 / 실행 이력
```

Tool Check에서 환경을 준비하고 Cell에 RTL과 테스트벤치를 추가하는 것으로 시작합니다.
시뮬레이션과 합성 결과를 검토한 뒤 backend, PDK와 제약을 지정해 Layout을 실행합니다.
각 단계의 결과를 확인하며 진행하고, 검증 지원 범위는 선택한 환경에 맞춰 확인합니다.

## 기술 구성

| 영역 | 구성 |
|---|---|
| 언어 | C++20 |
| 데스크톱 UI | Win32, 모니터별 DPI 지원 |
| 소스 편집기 | WebView2에서 실행하는 Monaco |
| 빌드 | Visual Studio / MSBuild |
| Linux 실행 | WSL2, 구조화된 프로세스 요청 |
| 시뮬레이션 | Verilator, Icarus Verilog, cocotb, GTKWave |
| 합성 / STA | Yosys, OpenSTA |
| 물리 설계 | OpenLane 2, OpenROAD Flow Scripts |
| 물리 검증 | Backend별 KLayout / Magic / Netgen 연동 |

## 시작하기

Windows 10/11 x64가 필요합니다. WSL2 자동 설정에는 Windows 10 버전 2004
(빌드 19041) 이상 또는 Windows 11이 필요합니다. 저장 공간은 앱, WSL2,
EDA 도구 환경과 설계 데이터에 사용할 수 있는 여유 공간 기준입니다.

| 구분 | CPU | 메모리 | 여유 저장 공간 |
|---|---|---|---|
| 최소 사양 | Intel Core i5-3470(Windows 10 기준 모델) 또는 SLAT·하드웨어 가상화를 지원하는 동급 x64 CPU | 4 GB 이상 | 50 GB 이상 |
| 권장 사양 | Intel Core i7 10세대 이상 | 32 GB 이상 | 50 GB 이상 |

최소 메모리 4 GB는 Microsoft의 Windows 하이퍼바이저 호스트 기준입니다.
WSL2를 시작하기 위한 기준이며 EDA 작업에 충분하다는 뜻은 아닙니다.
근거: Microsoft의 [WSL 설치 요구사항](https://learn.microsoft.com/en-us/windows/wsl/install),
[가상화 하드웨어 요구사항](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/host-hardware-requirements?pivots=windows-server).
[Intel 사양표](https://www.intel.com/content/www/us/en/products/sku/68316/intel-core-i53470-processor-6m-cache-up-to-3-60-ghz/specifications.html)에서
i5-3470의 VT-x와 EPT 지원을 확인했습니다. 이는 WSL2 구동 가능한 구체적인 CPU
기준이며 EDA 실행 성능을 검증했다는 뜻은 아닙니다. BIOS/UEFI에서 가상화를 켜야
하고, Windows 11에서는 별도로 [Microsoft의 지원 CPU 목록](https://support.microsoft.com/en-us/windows/experience/compatibility/windows-11-system-requirements)에
포함된 모델이 필요합니다.

내장 편집기에는 WebView2 Runtime이 필요하며,
NSIS 설치 프로그램은 Runtime이 없으면 Microsoft에서 내려받아 설치합니다.
앱 본체는 별도 Visual C++ Redistributable 설치 없이 실행됩니다.
EDA 실행에는 WSL2와 Linux 배포판, 설계에 사용할 도구·라이브러리·PDK를 준비합니다.

[설치 안내](docs/INSTALL.md)에서 환경 준비와 복구 방법을 확인하세요.
화면 사용법은 [Library Manager](docs/LIBRARY_MANAGER.md)와
[Workspace](docs/WORKSPACE.md)를 참고하세요. 관리형 도구 환경은 OpenLane 2
2.3.10과 ORFS 26Q2이며, PDK 및 독립 DRC/LVS 지원은 선택한 backend와 recipe에
따라 달라집니다.

## 빌드와 코드 구성

Visual Studio C++ 데스크톱 개발 도구, Windows SDK와 Node.js/npm을 사용합니다.
Developer PowerShell에서:

```powershell
msbuild Design++.slnx /m /p:Configuration=Debug /p:Platform=x64
msbuild Design++.slnx /m /p:Configuration=Release /p:Platform=x64
```

Release 실행 파일은 `x64/Release/Design++.exe`에 생성됩니다.
`src/`와 `include/designpp/`는 app, gui, application, core, runtime, adapters로
구분합니다. `editor/`는 Monaco 연동, `tests/`는 자동 테스트, `tools/`는 빌드·배포
스크립트, `docs/`는 사용자 안내와 설계 문서를 담습니다.

## 문서

- [단계별 사용 설명서](docs/guide/README.md)

- [문서 색인](docs/README.md)
- [아키텍처](docs/ARCHITECTURE.md) · [개발 계획](docs/PROJECT_PLAN.md)
- [프로젝트 형식](docs/PROJECT_FORMAT.md) · [Library 형식](docs/LIBRARY_FORMAT.md)
- [제3자 고지](THIRD_PARTY_NOTICES.md)

## 라이선스

Design++는 [MIT 라이선스](LICENSE)로 배포합니다. 함께 제공하는 제3자 구성
요소에는 각각의 라이선스가 적용되며, [제3자 고지](THIRD_PARTY_NOTICES.md)에
원문 위치를 정리했습니다.
