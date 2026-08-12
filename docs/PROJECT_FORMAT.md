# `.dpproj` schema v4

`project.dpproj`는 Cell 하나의 Design++ Workspace 설정을 저장하는 UTF-8 JSON
문서다. 파일 위치는 `<library>/cells/<cell-uuid>/project.dpproj`다.

## 필드

```json
{
  "schema_version": 4,
  "project_id": "UUID",
  "revision": 1,
  "library_id": "UUID",
  "cell_id": "UUID",
  "name": "cell display name",
  "top_module": "top",
  "created_utc": "ISO-8601 UTC",
  "modified_utc": "ISO-8601 UTC",
  "cpu_budget": 1,
  "source_policy": "auto_managed",
  "constraint_path": "",
  "toolchain_profile_id": "",
  "include_directories": [],
  "defines": [],
  "parameters": [],
  "source_overrides": [
    {
      "view_id": "UUID",
      "relative_path": "cells/.../files/top.sv",
      "enabled": true,
      "role": "rtl"
    }
  ],
  "testbench_configurations": [
    {
      "view_id": "UUID",
      "relative_path": "cells/.../files/top_tb.sv",
      "top_module": "top_tb",
      "backend": "icarus",
      "waveform_enabled": 1,
      "runner": "hdl",
      "cocotb_module": "",
      "cocotb_testcase": "",
      "waveform_format": "vcd"
    }
  ],
  "synthesis": {
    "liberty_paths": ["cells/.../files/stdcells.lib"],
    "flatten": 0
  },
  "timing": {
    "corner_name": "typical",
    "liberty_paths": ["cells/.../files/stdcells.lib"],
    "sdc_path": "cells/.../files/top.sdc"
  }
}
```

- ID 필드는 UUID이며 `revision`은 저장 때 증가한다.
- `source_policy` v1의 유일한 값은 `auto_managed`다.
- 경로는 Library 기준 상대 경로다. 절대 경로, `..` 탈출, Library 밖으로
  연결되는 reparse point는 거부한다.
- 빈 `constraint_path`와 `toolchain_profile_id`는 설정되지 않은 값이다.
- `cpu_budget`은 Windows/UI용 논리 CPU 하나를 남긴 범위에서 검증한다.

RTL, Testbench, Constraints 파일 목록은 `.dplib`가 소유한다. `.dpproj`는
`(view_id, relative_path)`별 include/exclude와 role override만 저장한다. 새 관리
파일은 자동 포함되고 삭제되거나 찾을 수 없는 파일은 Missing 진단 후 실행
입력에서 제외된다.

Workspace는 `.designpp/project.writer.lease`를 독점 획득한 경우에만 저장한다.
저장은 UUID 임시 파일을 flush한 후 atomic replace하며 직전 파일을
`project.dpproj.bak`으로 남긴다. 알 수 없는 schema, 손상된 JSON,
Library/Cell UUID 불일치는 원본을 변경하지 않는다.

schema v1은 Testbench와 synthesis 설정이 없는 v4 Project로 메모리에서
마이그레이션한다.
schema v2 Testbench 설정은 `runner=hdl`과 기존 `waveform_enabled`에 대응하는
`waveform_format=vcd|none`으로 마이그레이션한다. 원본은 열기만으로 갱신하지
않으며 다음 명시적 Save 때 `.bak`을 남기고 v4로 저장한다. schema v3은
기본 synthesis 설정(빈 Liberty 목록, flatten 해제)과 기본 timing 설정
(`typical`, 빈 Liberty/SDC)으로 메모리 migration한다. Testbench 설정은
`(view_id, relative_path)`별로 하나다. backend는 `icarus|verilator`, runner는
`hdl|cocotb`, waveform format은 `none|vcd|fst`를 지원한다. cocotb runner는
유효한 Python module 이름을 요구한다.

`synthesis.liberty_paths`, `timing.liberty_paths`, `timing.sdc_path`도 모두
Library 상대 safe path여야 한다. `.lib`는 Yosys의 선택적 area 매핑 입력이며,
`.sdc`와 timing corner는 OpenSTA vertical slice가 사용할 계약이다.
