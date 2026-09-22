# Design++ 단계별 사용 설명서

**Semiconductor IDE (Integrated Design Environment)**의 설계 작업 흐름을 설명합니다.
처음 사용하는 경우 1~4단계부터 진행하고, 목적에 따라 합성·타이밍 또는
Layout 단계로 이어갑니다. 각 문서는 준비물, 실제 화면의 실행 절차,
결과 확인과 실패 시 조치를 포함합니다.

| 단계 | 설명 |
|---|---|
| 01 | [환경 준비](01-environment.md) |
| 02 | [Library · Cell · View 구성](02-project.md) |
| 03 | [RTL 편집과 Lint](03-rtl-lint.md) |
| 04 | [시뮬레이션과 테스트벤치](04-simulation.md) |
| 05 | [논리 합성](05-synthesis.md) |
| 06 | [타이밍 분석](06-timing.md) |
| 07 | [PDK 선택과 Layout 실행](07-layout.md) |
| 08 | [DRC · LVS 물리 검증](08-verification.md) |
| 09 | [실행 기록 · 로그 · 취소와 복구](09-runs-recovery.md) |

## 작업을 시작하기 전에

- 편집한 입력을 저장하고 대상 Cell과 top module을 확인합니다.
- 화면의 버튼은 현재 작업, 입력과 지원 기능에 따라 비활성화될 수 있습니다.
- 도구 설치, PDK 인식, 프로세스 성공, 설계 검증 통과는 서로 다른 상태입니다.
- 이 안내는 현재 소스의 UI 및 실행 계약을 기준으로 작성했습니다.
  모든 PDK/도구 조합의 실제 검증을 뜻하지 않습니다.

[설치 안내](../INSTALL.md) · [지원 범위와 검증 기록](../RELEASE_NOTES_1.0.0.md)
· [English project overview](../../README.md) · [한국어 프로젝트 소개](../../README_ko.md)
