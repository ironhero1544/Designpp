# 06. 타이밍 분석

[사용 설명서 목차](README.md) · [이전 단계](05-synthesis.md) · [다음 단계](07-layout.md)

## 목표와 준비물

호환되는 합성 결과를 대상으로 OpenSTA 정적 타이밍 분석을 수행합니다.
합성 netlist, Liberty, SDC 제약과 OpenSTA가 필요합니다.

## 실행 순서

1. 대상 Cell의 **Timing** View를 엽니다.
2. 합성 결과가 없거나 호환되지 않으면 **Open Synthesis**로 이동해 합성합니다.
3. **Corner**, **SDC**, **Liberty**를 지정합니다. Corner 이름만 바꾼다고
   라이브러리 특성이 바뀌지 않으므로 실제 선택 파일도 확인합니다.
4. SDC의 clock 이름·대상 port·period가 RTL과 일치하는지 확인합니다.
   시간 단위는 사용하는 도구 및 라이브러리의 계약과 맞춰야 합니다.
5. **Run Timing**을 누릅니다.
6. 요약, 위반 경로와 **Runs**를 확인하고 **Reports / Artifacts / Script**로
   원본 결과와 실행 입력을 확인합니다.

## 결과 읽기

| 항목 | 해석 |
|---|---|
| Setup WNS | 가장 나쁜 setup slack. 음수이면 해당 조건의 위반 |
| Setup TNS | 음수 setup slack의 합 |
| Hold 관련 값 | hold 요구를 만족하는지 확인하는 값 |
| Startpoint / Endpoint | 보고된 경로의 시작점과 끝점 |

값이 없거나 N/A라면 0 또는 통과로 해석하지 않습니다. 프로세스 종료 성공과
타이밍 요구 만족은 별개이며, 제약이 빠진 분석으로 설계 전체를 판단하지 않습니다.

## 막혔을 때

- 합성 결과 불일치: 현재 RTL·설정으로 합성을 다시 실행합니다.
- clock을 찾지 못함: SDC의 port 이름과 합성 netlist를 비교합니다.
- 보고서가 비었거나 파싱 실패: 원본 OpenSTA 출력과 Script를 확인합니다.
- 위반 발생: clock 요구, 제약, 선택한 corner와 설계 경로를 검토합니다.

RTL, Liberty 또는 SDC를 변경한 뒤에는 영향을 받는 합성과 분석을 다시 수행합니다.

[사용 설명서 목차](README.md) · [이전 단계](05-synthesis.md) · [다음 단계](07-layout.md)
