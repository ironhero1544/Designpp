# 01. 환경 준비

[사용 설명서 목차](README.md) · [다음 단계](02-project.md)

## 목표와 준비물

Windows 앱에서 사용할 WSL 배포판, EDA 도구와 실행 환경을 확인합니다.
앱 설치와 VC++/WebView2 Runtime 준비는 [설치 안내](../INSTALL.md)를 먼저 따릅니다.
도구 다운로드에는 인터넷과 디스크 공간이 필요하며 WSL 초기 설정에는
관리자 승인 또는 Windows 재시작이 필요할 수 있습니다.

## 실행 순서

1. Library Manager의 **도구 > Tool Check**를 엽니다.
2. WSL2와 사용할 Linux 배포판을 확인합니다. 준비되지 않았다면 WSL 설정을 진행합니다.
3. 수행할 작업에 필요한 도구를 설치하고 재검사합니다. 모든 도구를 설치해야
   RTL 편집이나 개별 단계를 시작할 수 있는 것은 아닙니다.
4. OpenLane 2 또는 ORFS를 사용할 경우 해당 환경을 준비합니다.
   설치와 활성화는 별도 동작입니다. 검사가 통과한 환경을 선택해 활성화합니다.
5. 기존 도구를 사용할 경우 Toolchain Doctor에서 배포판과 경로를 확인합니다.
6. 설정을 변경했다면 기존 작업 창을 닫았다가 다시 열어 새 설정으로 시작합니다.

| 작업 | 확인할 도구 |
|---|---|
| Lint | Verilator |
| HDL 시뮬레이션 | 선택한 Icarus Verilog 또는 Verilator |
| Python 테스트벤치 | cocotb 환경과 선택한 simulator |
| 파형 보기 | GTKWave |
| 합성 / 타이밍 | Yosys / OpenSTA |
| Layout | 선택한 OpenLane 2 또는 ORFS 환경 |
| 물리 검증 | 해당 recipe가 요구하는 KLayout, Magic, Netgen 등 |

## 정상 결과

설치 로그의 종료 코드와 재검사 결과를 함께 확인합니다. Layout 실행 시에는
추가로 framework revision과 필수 기능을 probe합니다. Tool Check에서 도구를
찾았다는 사실만으로 특정 PDK의 DRC/LVS까지 지원되는 것은 아닙니다.

## 막혔을 때

- 설치 로그는 별도 Tool Check 창이 아니라 **Library Manager Output**에서 확인합니다.
- `capability_probe` 실패: 경로, 배포판, 원본 오류와 종료 코드를 확인합니다.
- 현재 managed 위치는 `~/.designpp/toolchains/environments/orfs-26Q2`와
  `~/.designpp/toolchains/environments/openlane2-2.3.10`입니다. 기존 설정이
  존재하지 않는 이전 경로를 가리키면 실제 설치 위치를 지정합니다.
- fingerprint 불일치: 환경을 재검사합니다. 임의로 값을 지우거나 검사를 우회하지 않습니다.
- 기존 설치가 있다는 메시지가 나왔다면 재설치보다 재검사·활성화 여부를 먼저 확인합니다.

[사용 설명서 목차](README.md) · [다음 단계](02-project.md)
