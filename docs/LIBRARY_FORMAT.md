# Design++ Library manifest v3

각 Library 루트의 `library.dplib`은 UTF-8 JSON 문서다. 현재
`schema_version`은 3이다. v1과 v2는 원본을 덮어쓰지 않고 메모리에서 v3으로
migration한다. 알 수 없는 schema이거나 필수 필드와 상대 경로가 유효하지
않으면 Design++는 원본을 변경하지 않고 해당 Library를 Invalid/read-only
상태로 취급한다. v3은 모든 Cell이 공유하는 technology Liberty 같은 관리
파일을 Library 수준 `files`에 저장한다.

```json
{
  "schema_version": 3,
  "library_id": "uuid",
  "revision": 1,
  "name": "Example",
  "description": "",
  "created_utc": "2026-01-01T00:00:00Z",
  "modified_utc": "2026-01-01T00:00:00Z",
  "files": [
    {
      "relative_path": "files/typical.lib",
      "role": "liberty",
      "size": 0,
      "modified_utc": "2026-01-01T00:00:00Z"
    }
  ],
  "cells": [
    {
      "cell_id": "uuid",
      "name": "counter",
      "description": "",
      "views": [
        {
          "view_id": "uuid",
          "name": "rtl",
          "description": "",
          "kind": "Verilog / SystemVerilog",
          "files": [
            {
              "relative_path": "cells/.../views/.../files/counter.sv",
              "role": "primary",
              "size": 0,
              "modified_utc": "2026-01-01T00:00:00Z"
            }
          ]
        }
      ]
    }
  ]
}
```

지원 View 종류는 `Verilog / SystemVerilog`, `Testbench`, `Constraints`,
`Synthesis`, `Timing`, `Layout`, `Report`다. Timing View의 SDC 후보는 같은
Cell의 관리 Constraints View에 있는 `.sdc`다. Liberty 후보는 Library 수준의
공유 `.lib`/`.liberty`이며, v2에서 가져온 Cell 관리 Liberty는 migration
호환 입력으로만 유지한다. 표시 이름 변경은 UUID 기반 저장 경로를 변경하지
않는다. 저장은 revision 비교 후 같은 디렉터리의 UUID 임시 파일을 flush하고
atomic replace하며 이전 manifest는 `.bak`으로 보존한다.
