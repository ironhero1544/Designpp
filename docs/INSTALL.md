# Design++ 설치 및 Toolchain 관리

## Windows 앱

NSIS 설치 프로그램 `Design++-1.0.0-x64-Setup.exe`는 현재 사용자에게
`%LOCALAPPDATA%\Programs\Design++`로 설치하고 시작 메뉴와 앱 제거 항목을
등록한다. 앱을 종료한 뒤 설치한다. 설치 프로그램 자체에는 관리자 권한이
필요하지 않지만 WSL 준비에는 별도 승인이 필요할 수 있다.

Windows 10/11 x64에서 Microsoft Visual C++ v14 x64 Redistributable과
Microsoft Edge WebView2 Runtime이 필요하다. 설치 프로그램은 이 런타임을
자동 다운로드하거나 설치하지 않는다. 배포판의 공식 Microsoft 설치 안내를
따라 준비한다.

Portable Release ZIP을 별도 폴더에 압축 해제하고 `Design++.exe`를 실행한다. 앱과
`WebView2Loader.dll`, `assets` 폴더는 같은 배포 폴더에 유지한다. Microsoft
Edge WebView2 Runtime이 없으면 Tool Check의 안내에 따라 설치한다.

## WSL과 검증된 EDA 환경

Tools > Tool Check에서 WSL2, Nix, OpenLane 2와 ORFS 상태를 확인한다. 환경
준비는 중앙 호환성 목록에 등록된 버전만 별도 불변 디렉터리에 설치한다.
설치 후 `선택 환경 활성화`를 눌러 revision, lock, 하위 도구와 명령 기능을
다시 검사한 뒤 선택한다. 실행 중인 Run은 시작할 때 선택한 환경을 유지한다.

현재 검증 계약은 다음과 같다.

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

## 업데이트와 제거

업데이트 전 모든 Design++ 창을 닫고 프로젝트를 백업한다. 설치 프로그램을
다시 실행하면 앱 파일을 갱신한다. ZIP은 새 폴더에 풀어 사용한다.
이전 버전으로 앱을 되돌리는 것과 Tool Check의 EDA 환경 롤백은 다르다.
새 schema로 저장한 데이터의 이전 앱 호환성은 보장하지 않는다.

Windows 설정의 앱 목록 또는 설치 폴더의 `Uninstall.exe`로 제거한다.
제거는 패키지 파일과 바로가기만 대상으로 하며 사용자 Library, Run,
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
개발 도구가 없는 깨끗한 Windows에서의 검증 여부는 릴리스 노트를 참조한다.
