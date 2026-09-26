# Design++ 설치 및 Toolchain 관리

## Windows 앱

NSIS 설치 프로그램 `Design++-1.0.0-x64-Setup.exe`는 현재 사용자에게
`%LOCALAPPDATA%\Programs\Design++`로 설치한다. 구성 요소 화면에서 시작 메뉴
바로가기(기본 선택), 바탕화면 바로가기(기본 해제), 사용자별 Windows App Paths
실행 경로(기본 해제)를 각각 선택할 수 있다. 앱 본체와 앱 제거 항목은 필수다.
App Paths를 선택하면 Windows 실행 창에서 `Design++.exe`로 앱을 열 수 있다.
전역·사용자 `PATH` 환경 변수는 변경하지 않는다. 무인 설치에는 기본 선택값을
적용한다. 앱을 종료한 뒤 설치한다.
설치 과정에서 Design++ [MIT 라이선스](../LICENSE)를 읽고 동의한다. ZIP에도
같은 라이선스 원문과 [제3자 고지](../THIRD_PARTY_NOTICES.md)가 포함된다.
설치 프로그램 자체에는 관리자 권한이 필요하지 않지만 WSL 준비에는 별도
승인이 필요할 수 있다.

Windows 10/11 x64에서 앱 본체는 Visual C++ Redistributable 설치 없이
실행되도록 Release C++ 런타임을 정적 링크한다. 내장 편집기에는 Microsoft
Edge WebView2 Runtime이 필요하다. NSIS 설치 프로그램은 설치 전 Runtime을
검사하고, 없으면 Microsoft의 Evergreen 부트스트래퍼를 내려받아 설치한다.
이 경우 인터넷 연결이 필요하며, 다운로드·설치 또는 설치 후 확인에 실패하면
Design++ 설치를 중단하고 원인을 표시한다. 이미 설치돼 있으면 다시 설치하지
않는다. 설치된 Runtime이 손상된 경우 앱의 Tool Check에서 복구한다.

Portable Release ZIP을 별도 폴더에 압축 해제하고 `Design++.exe`를 실행한다. 앱과
`WebView2Loader.dll`, `assets` 폴더는 같은 배포 폴더에 유지한다. Microsoft
Edge WebView2 Runtime이 없으면 Tool Check의 안내에 따라 설치한다.
ZIP 배포판은 Runtime을 자동 설치하지 않는다.

## WSL과 검증된 EDA 환경

Tools > Tool Check에서 WSL2, Nix, OpenLane 2와 ORFS 상태를 확인한다. 환경
준비는 중앙 호환성 목록에 등록된 버전만 별도 불변 디렉터리에 설치한다.
설치 후 `선택 환경 활성화`를 눌러 revision, lock, 하위 도구와 명령 기능을
다시 검사한 뒤 선택한다. 실행 중인 Run은 시작할 때 선택한 환경을 유지한다.
Doctor는 선택된 Profile의 경로를 검사하므로, 설치만 하고 활성화하지 않았거나
이전 Profile이 옛 경로를 유지하면 OpenLane·ORFS가 `Fail`일 수 있다. 기본
Profile의 옛 생성 경로는 메모리에서 현재 설치 경로로 보정하지만, 사용자
Profile과 직접 편집한 경로는 유지한다. 경로 입력란에서 수동 변경할 수 있다.
기본 Profile의 OpenLane PDK root는 `~/.volare`로 채워진다. 기존 기본
Profile의 빈 값도 읽을 때 메모리에서 보정하며, 직접 다른 경로를 입력해
저장할 수 있다. Doctor의 PDK 검사는 OpenLane PDK가 없더라도 설치된 ORFS의
`flow/platforms`에서 실행 가능한 platform을 찾으면 통과하고, 어느 쪽을
확인했는지 표시한다. PDK별 DRC/LVS 지원 여부는 PDK 관리에서 별도로 확인한다.

현재 검증 계약은 다음과 같다.

새 PC에서 **WSL2 자동 설정**을 실행하면 Ubuntu 설치, 최초 파일 시스템 초기화,
WSL2 변환과 기본 배포판 선택까지 처리한다. 새로 설치한 Ubuntu에는 비밀번호 없는
일반 `designpp` 사용자를 만들며, EDA 패키지 변경은 별도의 root 실행 단계가
담당한다. 기존 Ubuntu가 있으면 기존 Linux 사용자 설정은 유지한다.
기존 WSL 배포판이 감지되면 목록에서 기존 WSL2 배포판 또는 별도
`DesignPlusPlus` Ubuntu 설치를 직접 선택한다. 기존 배포판을 선택하면 내부
파일이나 사용자를 변경하지 않고 WSL 업데이트와 실행 확인을 거친 후 Windows의
기본 WSL 배포판으로 지정한다. 새 환경은 기존 Ubuntu를 덮어쓰지 않는다.
기존 Toolchain Doctor 프로필에 배포판 이름을 명시했다면 선택 후 그 프로필의
`WSL distribution`도 같은 이름으로 지정하고 저장해야 한다.
Windows 10의 WSL이 이름 지정 설치(`--name`)를 지원하지 않으면 설치 단계가
실패하며 기존 배포판은 그대로 유지된다. 새 환경의 최초 초기화가 재부팅으로
중단되면 같은 새 환경 선택으로 이어간다. 이미 완성된 `DesignPlusPlus`가 있으면
목록에서 기존 배포판으로 선택하며, 새 환경 선택은 중복 설치를 거부한다.
WSL 자체는 `wsl.exe --update`로 갱신한 뒤 Ubuntu를 시작하며, 마지막 검사에서
WSL2 커널 확인 표식이 없거나 `E_UNEXPECTED`가 나오면 완료로 처리하지 않는다.
Toolchain Doctor에서 WSL 오류가 표시되면 `Repair WSL...` 버튼으로 같은 설정을
시작하고, 완료 후 Doctor를 다시 실행한다.
UAC 승인 뒤 `Design++ 관리자 설정` PowerShell 창이 열려 Windows/WSL 설치
진행을 표시하며 관리자 단계가 끝나면 자동으로 닫힌다.
관리자 단계가 실패하면 실패한 WSL 명령과 종료 코드를 표시하고 Enter를 누를 때까지
창을 유지한다. 새 설치에서는 Ubuntu 설치 후 WSL 기본 버전을 2로 지정한다.
이 창의 오류 문구를 확인한 뒤 Tool Check에서 다시 실행한다.

Windows 기능을 처음 켠 직후 Ubuntu 시작이 실패하면 재부팅 알림에서 즉시
재부팅하거나 나중에 재부팅할 수 있다. 설치 pending 표식이 유지되므로 재부팅 후
같은 설정을 다시 실행하면 전용 일반 사용자 생성부터 안전하게 이어진다.
`WSL2 Linux 실행 확인`은 `Ubuntu`를 명시하므로 다른 기본 배포판의 상태와
혼동하지 않는다.

| Provider | Bundle | 명령 계약 |
|---|---|---|
| OpenLane 2 | `openlane2-2.3.10` | `openlane2-classic-v1` |
| ORFS | `orfs-26Q2` | `orfs-26q2-v1` |

검사 실패 시 현재 활성 환경은 유지된다. 이전 환경으로 되돌릴 때는 Tool
Check에서 해당 provider를 선택하고 `이전 환경 롤백`을 누른다. 롤백 대상도
현재 호환성 검사를 통과해야 한다. `미지원 버전`, `revision 불일치`, `lock
변경`, `필수 기능 누락`은 서로 다른 진단으로 표시된다.

OpenLane과 ORFS 환경 및 PDK는 ZIP에 포함되지 않는다. Tool Check의 명시적인
환경 준비 작업으로 설치하며, 앱 실행이나 일반 검사는 다운로드 또는 기존
checkout 변경을 수행하지 않는다.
ORFS 설치 중 OpenROAD·Yosys·EQY는 고정 commit의 얕은 checkout을 사용한다.
Nix에는 검증한 후보의 로컬 `path:` 입력으로 전달해 중첩 Git submodule의
`revCount` 계산 오류로 설치가 중단되지 않게 한다. 실패하거나 취소된
OpenLane·ORFS 후보 checkout은 설치 잠금을 보유한 정리 단계에서 제거한다.
비정상 종료로 남은 Design++ 후보는 다음 설치 또는 Tool Check의
**빌드 캐시 정리**에서 회수한다. 완료된 환경은 유지한다.
새로 준비한 ORFS의 완료 표식은 이 입력 방식을 기록한다. 기존에 완료된 ORFS는
원래의 Git 입력과 이미 준비된 Nix 결과를 계속 사용하므로 재설치할 필요가 없다.
ORFS 개별 설치와 전체 EDA Toolchain 설치는 별도 확인 화면에서 로컬 소스
빌드를 허용할지 묻는다. 허용하면 캐시에 없는 의존 도구를 WSL에서 Nix 작업
1개로 컴파일하며, 사용 가능한 논리 CPU가 둘 이상이면 하나를 남긴다.
이 작업은 오래 걸리고 디스크
공간을 많이 사용할 수 있다. 거절하면 설치를 시작하지 않는다. 설치가
실패하거나 취소돼도 현재 활성 환경은 유지된다. 일반 검사와 Run은 계속
offline/cache-only로 동작하며, 캐시에 없는 도구를 자동 빌드하지 않는다.
**빌드 캐시 정리**는 Design++가 남긴 실패 후보 checkout만 삭제한다.
공유 Nix 저장소의 빌드 결과는 여러 환경이 참조할 수 있어 삭제하지 않는다.

## 업데이트와 제거

업데이트 전 모든 Design++ 창을 닫고 프로젝트를 백업한다. 설치 프로그램을
다시 실행하면 앱 파일을 갱신한다. ZIP은 새 폴더에 풀어 사용한다.
이전 버전으로 앱을 되돌리는 것과 Tool Check의 EDA 환경 롤백은 다르다.
새 schema로 저장한 데이터의 이전 앱 호환성은 보장하지 않는다.

Windows 설정의 앱 목록 또는 설치 폴더의 `Uninstall.exe`로 제거한다.
제거는 패키지 파일과 선택하여 설치한 바로가기·App Paths 등록만 대상으로
하며 사용자 Library, Run,
`%LOCALAPPDATA%\DesignPlusPlus` 설정과 WSL 환경은 삭제하지 않는다.
사용자가 설치 폴더에 추가한 파일이 남으면 폴더도 유지된다.

## Layout 시작 실패 진단

- `capability_probe`: Output의 원본 진단과 종료 코드를 확인한다.
- 도구가 설치돼도 선택한 경로가 없으면 실행할 수 없다. 기본 managed 위치는
  `~/.designpp/toolchains/environments/orfs-26Q2`와
  `~/.designpp/toolchains/environments/openlane2-2.3.10`이다.
- 이전 `~/.designpp/toolchains/orfs` 또는 `openlane2` 경로를 사용하던 경우,
  Toolchain Doctor의 경로를 확인하거나 Tool Check에서 검증 환경을 활성화한다.
  경로가 존재하는지 확인하기 전에 기존 설정을 일괄 덮어쓰지 않는다.
- fingerprint 불일치는 재검사하여 변경 원인을 확인한다. 호환성 검사를 우회하거나
  `verified` 값을 수동 편집하지 않는다.
- 경로 설정을 바꾼 후 열려 있던 Layout 창은 닫았다가 다시 연다.

## 배포 확인

ZIP과 설치 EXE 각각에 `.sha256` 파일을 제공한다. PowerShell의
`Get-FileHash -Algorithm SHA256 <파일>` 결과를 비교한다. 현재 산출물은
코드 서명되지 않았으므로 게시자를 인증하는 서명이 없다.
별도 PC의 처음 사용자 환경에서 WSL·EDA 도구 준비와 앱 사용을 포함한 초기
설치 테스트는 2026-09-26 사용자 확인으로 완료했다. 개발 도구가 없는
Windows 11의 압축 해제·실행·재시작과 WebView2 Runtime이 없는 Windows 10의
자동 설치 경로는 각각 독립 조건으로 아직 검증하지 않았다.
