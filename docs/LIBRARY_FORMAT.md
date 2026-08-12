# Design++ Library manifest v1

각 Library 루트의 `library.dplib`은 UTF-8 JSON 문서다. `schema_version`이 1이
아니거나 필수 필드와 상대 경로가 유효하지 않으면 Design++는 원본을 변경하지
않고 해당 Library를 Invalid/read-only 상태로 취급한다.

```json
{
  "schema_version": 1,
  "library_id": "uuid",
  "revision": 1,
  "name": "Example",
  "description": "",
  "created_utc": "2026-01-01T00:00:00Z",
  "modified_utc": "2026-01-01T00:00:00Z",
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
`Synthesis`, `Layout`, `Report`다. 표시 이름 변경은 UUID 기반 저장 경로를
변경하지 않는다. 저장은 revision 비교 후 같은 디렉터리의 UUID 임시 파일을
flush하고 atomic replace하며 이전 manifest는 `.bak`으로 보존한다.
