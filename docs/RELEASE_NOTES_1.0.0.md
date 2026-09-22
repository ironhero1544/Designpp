# Design++ 1.0.0 릴리스 노트

## 배포 범위

Windows x64 앱, WebView2 loader, 번들 Monaco 자산, 제3자 고지와 설치 안내를
ZIP 및 NSIS 설치 EXE로 제공한다. WSL2, EDA, PDK와 VC++/WebView2 Runtime은
별도 준비 항목이다. 외부 공개 업로드와 코드 서명은 이 산출물에 포함하지 않는다.

## 최근 안정화

- 백틱 셸 디버거 및 tmux 실행 래퍼 제거, 직접 WSL 실행 복원
- OpenLane/ORFS 설치의 로그인 종료 훅에 의한 잘못된 종료 코드 수정
- Layout custom 경로 해시와 실제 환경 fingerprint의 잘못된 비교 제거
- probe 실패에 종료 코드와 경로 누락 안내 추가
- Library Manager 진행률 표시 모드 전환 및 다시 그리기 보완

## 검증과 제한

- 실제 managed ORFS 26Q2 경로의 호환성 probe 통과:
  `Tlqkf_IRONHERO_2026-09-22_10_10_56_net40.trx`.
- 같은 환경에서 격리된 ASAP7 합성, Verilog 및 ODB 생성 통과:
  `Tlqkf_IRONHERO_2026-09-22_15_29_15_net40.trx`.
- 관련 환경/flow 서비스 테스트 23개 통과:
  `Tlqkf_IRONHERO_2026-09-22_10_13_00_net40.trx`.
- Timing 취소 테스트의 시작 등록/handle 인계 관측 오류를 수정했다.
  가짜 실행기의 handle 반환을 gate로 제어하고 실제 취소 통지까지 기다린다.
  인계 전 취소와 일반 경합을 8회 수행하며 중복 callback의 완료 1회를 검증한다.
  서비스 오류를 숨기도록 timeout을 늘리거나 재시도하지 않았다.
- 조건부 LVS 테스트의 미실행은 실제 엔진 통과와 구분한다.
- 사용자 Timer의 전체 배치·배선, 모든 PDK 조합, 깨끗한 Windows에서
  설치부터 WSL 준비와 예제 실행까지는 미검증이다.
- ASAP7 독립 LVS와 일반적인 로그 검색/QoR 비교는 제공 범위 밖이다.

## 1.0.0 패키지 검증 결과

- x64 Debug/Release 빌드 통과. EXE ProductVersion `1.0.0` 확인.
- Debug 전체 285/285 runner 통과:
  `Tlqkf_IRONHERO_2026-09-22_15_42_07_net40.trx`.
- Release 전체 285/285 runner 통과:
  `Tlqkf_IRONHERO_2026-09-22_15_42_52_net40.trx`.
- 위 집계에는 환경 변수 미설정으로 조기 반환하는 실제 LVS 항목 1개가 포함된다.
  따라서 자동 실행 284개 통과와 opt-in LVS 미실행 1개로 구분한다.
- NSIS 3 컴파일 경고 없이 완료. ZIP/EXE와 SHA-256 파일 생성.
- 현재 개발 PC의 고유 테스트 폴더에서 무인 설치, 모든 payload 파일의 해시 일치,
  재설치, 버전 등록, 설치된 앱의 창 생성과 정상 종료를 확인했다.
- 제거 후 앱 payload·시작 메뉴·제거 레지스트리가 사라지고 사용자 추가 파일이
  남는 것을 확인했다. 사용자 프로젝트 및 WSL 설치는 변경하지 않았다.
- 이 로컬 검증은 개발 도구/런타임이 없는 Windows VM 검증을 대신하지 않는다.
  코드 서명 및 외부 공개 업로드는 수행하지 않았다.
