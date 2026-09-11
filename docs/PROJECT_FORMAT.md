# `.dpproj` schema v12

`project.dpproj`는 Cell 하나의 Design++ Workspace 설정을 저장하는 UTF-8 JSON
문서다. 파일 위치는 `<library>/cells/<cell-uuid>/project.dpproj`다.

## 필드

```json
{
  "schema_version": 12,
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
  "physical_verification": {
    "drc_recipe_id": "",
    "lvs_recipe_id": "",
    "top_cell": "",
    "power_net": "",
    "ground_net": "",
    "parameters_json": "{}"
  },
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
  },
  "physical_implementation": {
    "backend_id": "openlane2",
    "pdk": "sky130A",
    "standard_cell_library": "sky130_fd_sc_hd",
    "clock_ports": ["clk"],
    "clock_period_ns": "10.0",
    "core_utilization_percent": 40,
    "placement_density_percent": "",
    "die_area": [],
    "core_area": [],
    "tap_cell_distance_um": "",
    "io_placement": {
      "algorithm": "matching",
      "minimum_distance_um": "",
      "vertical_length_um": "",
      "horizontal_length_um": "",
      "vertical_thickness_multiplier": "",
      "horizontal_thickness_multiplier": "",
      "vertical_extension_um": "",
      "horizontal_extension_um": "",
      "vertical_layer": "",
      "horizontal_layer": "",
      "unmatched_policy": "both",
      "north": {"minimum_distance_um": "", "bit_major": 0, "entries": []},
      "south": {"minimum_distance_um": "", "bit_major": 0, "entries": []},
      "east": {"minimum_distance_um": "", "bit_major": 0, "entries": []},
      "west": {"minimum_distance_um": "", "bit_major": 0, "entries": []}
    },
    "pnr_sdc_path": "",
    "signoff_sdc_path": "",
    "power_distribution": {
      "multilayer": 1,
      "core_ring": 0,
      "enable_rails": 1,
      "vertical_width_um": "",
      "horizontal_width_um": "",
      "vertical_spacing_um": "",
      "horizontal_spacing_um": "",
      "vertical_pitch_um": "",
      "horizontal_pitch_um": "",
      "vertical_offset_um": "",
      "horizontal_offset_um": ""
    },
    "advanced_overrides_json": "{}",
    "orfs": {
      "platform": "sky130hd",
      "flow_variant": "base",
      "advanced_variables_json": "{}"
    }
  }
}
```

- ID 필드는 UUID이며 `revision`은 저장 때 증가한다.
- `source_policy` v1의 유일한 값은 `auto_managed`다.
- 경로는 Library 기준 상대 경로다. 절대 경로, `..` 탈출, Library 밖으로
  연결되는 reparse point는 거부한다.
- Cell ID와 같은 Cell 안의 View ID는 Windows 경로와 동일하게 대소문자를
  구분하지 않는 고유 식별자여야 한다. 중복 ID는 서로 다른 Cell/View가 같은
  `project.dpproj` 또는 View 디렉터리를 공유할 수 있으므로 거부한다.
- 빈 `constraint_path`와 `toolchain_profile_id`는 설정되지 않은 값이다.
  `toolchain_profile_id`가 있으면 이 Cell은 해당 per-user Toolchain Profile에
  고정되며, 프로필이 없을 때 다른 Cell의 선택 프로필로 조용히 대체하지 않는다.
  비어 있는 경우에만 Toolchain Doctor의 현재 선택 프로필을 사용한다. 프로필은
  설치된 WSL/EDA 도구를 가리키는 전역 자원이고, PDK/SCL·floorplan·I/O·PDN
  같은 물리 구현 설정은 아래 `physical_implementation`에 저장되는 Cell 전용
  값이다.
- `cpu_budget`은 Windows/UI용 논리 CPU 하나를 남긴 범위에서 검증한다.

RTL, Testbench, Constraints 파일 목록은 `.dplib`가 소유한다. `.dpproj`는
`(view_id, relative_path)`별 include/exclude와 role override만 저장한다. 새 관리
파일은 자동 포함되고 삭제되거나 찾을 수 없는 파일은 Missing 진단 후 실행
입력에서 제외된다.

Workspace는 `.designpp/project.writer.lease`를 독점 획득한 경우에만 저장한다.
저장은 UUID 임시 파일을 flush한 후 atomic replace하며 직전 파일을
`project.dpproj.bak`으로 남긴다. 알 수 없는 schema, 손상된 JSON,
Library/Cell UUID 불일치는 원본을 변경하지 않는다.

schema v1은 Testbench와 synthesis 설정이 없는 v10 Project로 메모리에서
마이그레이션한다.
schema v2 Testbench 설정은 `runner=hdl`과 기존 `waveform_enabled`에 대응하는
`waveform_format=vcd|none`으로 마이그레이션한다. 원본은 열기만으로 갱신하지
않으며 다음 명시적 Save 때 `.bak`을 남기고 v10로 저장한다. schema v3은
기본 synthesis 설정(빈 Liberty 목록, flatten 해제)과 기본 timing 설정
(`typical`, 빈 Liberty/SDC)으로 메모리 migration한다. Testbench 설정은
`(view_id, relative_path)`별로 하나다. backend는 `icarus|verilator`, runner는
`hdl|cocotb`, waveform format은 `none|vcd|fst`를 지원한다. cocotb runner는
유효한 Python module 이름을 요구한다.

schema v5의 `openlane` 설정은 schema v6의 tool-neutral
`physical_implementation`으로 migration한다. schema v7은 PDN 설정을 추가했고,
schema v8은 선택적 `tap_cell_distance_um`을 추가한다. 빈 값은 PDK/SCL 기본
tap-cell column distance를 사용하며, 값이 있으면 OpenLane
`FP_TAPCELL_DIST`(µm)로 전달한다.

schema v9은 구조화된 `io_placement`를 추가한다. v1~v8 문서는 자동 배치
`matching`, unmatched 정책 `both`, 빈 optional 값과 빈 방향별 pin-order로
메모리 migration한다. 방향별 배열은 run 준비 시 결정적인 `pin_order.cfg`로
생성되며 프로젝트나 Library 관리 파일로 추가되지 않는다. 폼에 없는 안전한
OpenLane 변수는 flat typed JSON인 `advanced_overrides_json`에만 남고, managed
경로·실행·RTL·SDC·pin-order 변수는 저장 전에 거부한다.

schema v11은 `physical_implementation.automatic_fields` 배열에 scalar 자동값
선택을 저장한다. optional 누락은 backend/PDK 기본값이다. v1~v10 scalar
값은 명시값으로 보존하며 메모리 migration만 수행한다. 빈 폼 입력은 이전
override를 제거한다. utilization 자동값은 40%, clock이 있을 때 period
자동값은 10 ns다. ORFS 고정 Die/Core에서는 utilization을 출력하지 않는다.
PDN boolean 자동값도 실행 config에서 생략한다.

schema v12는 Cell별 `physical_verification` 선택을 추가한다. recipe ID, top
cell, 전원·접지 매핑과 flat parameter JSON만 프로젝트에 저장하며 실행 코드나
규칙 파일 경로는 저장하지 않는다. 실제 규칙 snapshot과 신뢰 상태는 per-user
ToolchainSettings registry가 소유한다. v1~v11은 빈 검증 설정으로 메모리
migration하며 로드만으로 원본을 덮어쓰지 않는다.

schema v10은 `physical_implementation.orfs`에 ORFS platform, flow variant,
flat advanced variables를 추가한다. v1~v9 문서는 `sky130hd`/`base`/`{}`를
기본값으로 메모리 migration하며 원본을 자동으로 덮어쓰지 않는다. backend
전환은 반대 backend의 설정을 삭제하지 않는다. ORFS의 managed
`DESIGN_CONFIG`, `DESIGN_NAME`, `VERILOG_*`, `SDC_FILE`, `PLATFORM`,
`FLOW_VARIANT`, `WORK_HOME`, 결과 디렉터리, I/O/PDN Tcl 경로와 `RUN_*`
변수는 JSON 편집기에서 거부한다. ORFS config.mk, SDC, stage ODB와 lineage
manifest는 Library가 아니라 RunStore에만 보존한다.

`synthesis.liberty_paths`, `timing.liberty_paths`, `timing.sdc_path`도 모두
Library 상대 safe path여야 한다. `.lib`는 Yosys의 선택적 area 매핑 입력이며,
`.sdc`와 timing corner는 OpenSTA vertical slice가 사용할 계약이다.
