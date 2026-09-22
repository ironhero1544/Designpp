# 05. 논리 합성

[사용 설명서 목차](README.md) · [이전 단계](04-simulation.md) · [다음 단계](06-timing.md)

## 목표와 준비물

RTL을 합성해 netlist와 회로 구조를 확인합니다. 저장된 RTL과 top module,
Yosys, 설계에 맞는 Technology Liberty가 필요합니다.

## 실행 순서

1. 대상 Cell의 **Synthesis** View를 엽니다.
2. **Synthesis Setup**의 Top Module과 입력 상태를 확인합니다.
3. **Technology Liberty** 목록에서 사용할 셀 라이브러리를 선택합니다.
   Liberty 항목이 없다면 해당 기술 라이브러리가 프로젝트에 준비됐는지 먼저 확인합니다.
4. 필요할 때 **Flatten hierarchy**를 선택합니다. 계층 유지 여부가 바뀌면
   회로 탐색 형태와 후속 입력이 달라질 수 있습니다.
5. **Run Synthesis**를 누릅니다. 로그는 Library Manager Output에서도 확인할 수 있습니다.
6. 완료 후 **Problems**, **Report**, **Runs**를 확인합니다.
7. **Readable / Gate** 표시와 계층 탐색으로 결과 구조를 살펴봅니다.
   **Reports**, **Artifacts**, **Script**로 원본 결과와 실행 입력을 확인합니다.

## 정상 결과

도구 종료 성공과 필수 artifact 생성이 모두 확인되어야 합니다.
합성 결과를 Timing에 사용할 때는 RTL·설정·라이브러리와 호환되는 Run이어야 합니다.
기존 합성 이후 RTL이나 설정을 바꿨다면 다시 합성합니다.

## 막혔을 때

- 셀 매핑 실패: Liberty 내용과 선택, 지원되는 셀/기능, Yosys 로그를 확인합니다.
- top 또는 하위 module 누락: RTL View의 입력 목록을 확인합니다.
- Run은 있으나 후속 단계가 거부됨: 입력이 변경되었는지 확인하고 새 Run을 만듭니다.
- 회로 표시의 한계나 생략은 합성 실패와 구분합니다. 원본 netlist와 보고서도 확인합니다.

Layout backend는 자체 합성 흐름을 사용할 수 있습니다. 이 View에서 만든 결과가
모든 Layout 실행에 그대로 재사용되는 것으로 가정하지 않습니다.

[사용 설명서 목차](README.md) · [이전 단계](04-simulation.md) · [다음 단계](06-timing.md)
