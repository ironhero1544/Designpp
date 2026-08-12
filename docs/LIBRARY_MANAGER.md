# Library Manager

Library Manager는 Design++ 실행 시 가장 먼저 열리는 메인 Win32 창이다.
기존 목업의 좌측 탐색기, 중앙 목록, 하단 Output 레이아웃만 유지하면서 WSL2
실행 환경과 EDA 도구 설정을 소유한다. 프로젝트 트리와 Flow Stage 목업
데이터는 표시하지 않는다. 실제 라이브러리 저장 기능이 연결되기 전까지 좌측
탐색기와 중앙 `Library / Type / Status` 목록은 비어 있다. 하단 Output은 모든
작업의 통합 로그 콘솔 역할을 한다.

## 제공 기능

- WSL2/Ubuntu 자동 설정
- 기본 APT 도구와 Design++ 전용 Python 가상환경 설치
- 표준 Win32 `도구 > Tool Check` 메뉴에서 독립 Tool Check 창 열기
- Tool Check 창에서 13개 도구의 실행 가능 여부와 버전 병렬 검사
- 단계별 진행률, 종료 코드, 실시간 stdout/stderr 로그
- 실행 중 작업 및 하위 프로세스 트리 취소
- Per-Monitor V2 high-DPI 배율과 런타임 DPI 변경 대응

도구 검사는 논리 CPU를 하나 남겨 두고 최대 4개의 probe만 동시에 실행한다.
Tool Check 창에는 로그 컨트롤을 두지 않는다. 도구 검사, WSL2 설정, 패키지
설치 로그는 모두 Library Manager 메인 창에 표시한다. 각 Library Manager
프로세스는 독립 상태와 작업 세션을 가지며, 작업 스레드는 Win32 컨트롤에
직접 접근하지 않고 이벤트 큐를 통해 UI 스레드에 결과를 전달한다.

## WSL2 자동 설정

사용자 확인 후 관리자 권한이 없으면 UAC를 통해 Design++를 다시 실행한다.
관리자 프로세스는 다음 작업을 순차 실행한다.

1. 설치된 배포판을 확인하고 Ubuntu가 없을 때만 `wsl --install` 실행
2. 새 배포판의 기본 버전을 WSL2로 설정
3. Ubuntu에서 Linux 커널 실행 확인

Windows 선택 기능을 처음 활성화하는 시스템에서는 재부팅이 필요할 수 있다.
이 경우 재부팅 후 Library Manager를 다시 열어 설정과 도구 검사를 계속한다.

## Toolchain 설치 및 업데이트

사용자 확인 후 공급자별 단계를 순차 실행한다.

- APT: Verilator, Icarus Verilog, Yosys, GTKWave, Magic, Netgen LVS
  (`netgen-lvs`), KLayout,
  Python 3, venv, curl, Git 및 Make
- Python: `$HOME/.designpp/venv`에 cocotb 설치
- Nix: OpenLane 공식 설치 명령과 binary cache 설정
- OpenLane 2: `$HOME/.designpp/toolchains/openlane2`에 clone/update 후 smoke test
- ORFS: `$HOME/.designpp/toolchains/orfs`에 recursive clone/update

설치 단계가 끝나면 전체 도구 검사를 자동으로 다시 실행해 목록과 버전을
갱신한다.

### Tool Check 개별 작업

Tool Check에서 한 행을 선택하면 `선택 설치 / 업데이트` 또는 `선택 삭제`를
실행한다. 선택된 행이 없으면 버튼이 `전체 설치 / 업데이트`와 `전체 삭제`로
바뀌고 전체 관리 도구를 대상으로 동작한다. 모든 변경은 확인 대화상자를
거치고 메인 Output에 기록되며, 완료 후 자동 재검사한다. 전체 삭제는 APT EDA
패키지, Design++ cocotb 가상환경, OpenLane 2 및 ORFS checkout을 제거하지만
공유 Nix provider와 외부 Docker Desktop은 유지한다.

| 도구 유형 | 개별 설치 | 개별 삭제 |
|---|---|---|
| APT 도구 | 해당 패키지만 설치/업데이트 | 해당 패키지만 `apt remove` |
| cocotb | Design++ venv에 설치/업데이트 | venv에서 cocotb만 제거 |
| OpenLane 2 | Nix provider와 checkout 설치/업데이트 | Design++ checkout만 제거 |
| ORFS | Design++ checkout clone/update | Design++ checkout만 제거 |
| OpenROAD/OpenSTA | OpenLane 2 managed environment 설치 | 공유 환경이므로 차단 |
| Nix | 공식 provider 설치 | 공유 provider이므로 차단 |
| Docker Desktop | 외부 소유이므로 차단 | 외부 소유이므로 차단 |

삭제 명령은 사용자 경로나 선택 문자열을 shell source에 삽입하지 않고
`$HOME/.designpp/toolchains/openlane2`와 `orfs`의 고정 관리 경로만 대상으로
한다.

OpenSTA/OpenROAD는 OpenLane 2 Nix 환경 안에서 검사한다. Docker는 권장 Nix
방식이 동작하지 않을 때 사용하는 선택적 대체 provider이므로 Docker Desktop
자체는 자동 설치하지 않는다. PDK 설치는 아직 별도 후속 작업이다.

Ubuntu의 `netgen` 패키지는 LVS 도구가 아니라 3D 메시 생성기이므로 설치 및
검사 대상으로 사용하지 않는다. Design++는 `netgen-lvs` 패키지를 관리하고
`dpkg-query`로 버전을 검사한다. 사용자가 별도로 설치한 메시 생성기 `netgen`은
삭제하지 않는다.

WSL Windows 기능 설정만 UAC가 필요한 elevated child process로 실행한다.
Library Manager GUI 전체를 관리자 권한으로 다시 실행하지 않으므로 일반 권한
메인 창이 중복 생성되지 않는다. elevated 단계의 명령과 종료 코드는 메인
Output에 기록하고, 일반 WSL 설치 단계의 stdout/stderr는 실시간 스트리밍한다.

현재 기본 배포판을 대상으로 실행한다. 배포판 선택, OpenLane/ORFS 설치 경로,
PDK root 및 Toolchain Profile 저장은 `PROJECT_PLAN.md` Phase 1의 후속 작업이다.

## 안전성과 제한

- 설치 작업은 항상 사용자 확인 뒤 시작한다.
- 일반 도구 검사는 시스템을 변경하지 않는다.
- WSL 관리 명령은 UTF-16LE, Linux 도구 출력은 UTF-8로 처리한다.
- 창을 닫거나 취소하면 Windows Job Object를 통해 실행 중 프로세스 트리를
  종료한다.
- 비밀번호, 토큰 또는 사용자 입력을 shell source에 삽입하지 않는다.
