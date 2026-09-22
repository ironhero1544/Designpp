# Design++ 설치 및 Toolchain 관리

## Windows 앱

Release ZIP을 별도 폴더에 압축 해제하고 `Design++.exe`를 실행한다. 앱과
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
