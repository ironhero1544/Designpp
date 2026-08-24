# Cell Workspace

Workspace는 Library Manager에서 View를 더블클릭할 때 열리는 독립 Win32
top-level 창이다. 창 유일성 키는 `(library_id, cell_id)`다. 같은 Cell의 다른
View는 기존 창에서 전환하고 다른 Cell은 별도 창으로 연다.

좌측 Source 패널은 `RTL Sources / Testbench / Constraints → View → 폴더 → 파일`
계층의 Win32 TreeView로 Source Set을 표시한다. 파일과 상위 노드의 체크박스로
Lint include/exclude를 변경하고, 파일을 더블클릭하면 Monaco에서 연다. `Add
Files...`는 선택한 View의 관리 폴더로 외부 파일을 복사하고 `.dplib`, Source Set,
module 후보를 worker에서 함께 갱신한다. 외부 파일 링크는 만들지 않으며 동일한
파일 이름이나 writer lease/revision 충돌 시 원본과 manifest를 보존한다.

중앙에는 WebView2로 호스팅한 Monaco Editor, 우측 inspector에는 top module, include directory, define,
parameter, constraint, CPU budget 설정을 표시한다. 하단에는 Problems, Runs,
Artifacts, Cell별 Output을 둔다. Workspace마다 WebView2 controller와 Monaco
instance 하나를 만들고 열린 파일은 독립 model과 tab으로 관리한다.
좌측 Sources, 중앙 editor, 우측 inspector 및 하단 pane의 경계는 DPI에 맞춰
크기가 조정되는 draggable splitter이며 작은 창에서도 editor 최소 폭을 보존한다.

편집 대상은 Library가 관리하는 `.v`, `.vh`, `.sv`, `.svh` UTF-8 파일이다.
Source 더블클릭은 파일을 열고 동일 파일은 기존 tab을 활성화한다. Monaco는
undo/redo, find/replace, go-to-line, selection/scroll/undo state를 model별로
유지한다. 저장은 Ctrl+S 또는 Save All로만 수행하며 autosave하지 않는다.
SystemVerilog/Verilog 강조 규칙은 저장소의 Monarch tokenizer가 제공한다.

Inspector의 빈 설정은 Source Set에서 안전하게 추론한다. module 후보가 하나면
자동으로 top module로 사용하고, 여러 후보 중 Cell 이름과 일치하는 module을
우선한다. 후보가 아직 없는 빈 RTL에서는 Cell 이름을 초기 top module로 제안한다.
선택된 module의 값 parameter 기본값, `.vh/.svh` 관리 폴더, 유일한 Constraints
파일도 각각 Parameters, Include directories, Constraint path에 채운다. 기존 사용자
값은 덮어쓰지 않으며 Define은 설계 의미를 바꿀 수 있어 임의 생성하지 않는다.

편집 중인 `.v/.sv`에 module 선언이 정확히 하나 있으면 저장 시 관리 파일 이름을
해당 module 이름과 일치시킨다. 예를 들어 `module half_adder`를 저장하면 기존
`verilog.sv`는 같은 View 안의 `half_adder.sv`로 이동하고 `.dplib`, Source Set,
열린 탭 및 파일별 project override 경로를 함께 갱신한다. 여러 module이 한 파일에
있거나 대상 이름이 충돌하면 자동 변경하지 않거나 명시적인 오류로 저장을 중단한다.

첫 열기에 `project.dpproj`가 없으면 Cell 메타데이터로 기본 프로젝트를 만든다.
손상, 미지원 schema, lease 경쟁 또는 생성 실패 시 원본 View를 변경하지 않고
read-only 정보 모드로 연다. 변경은 제목의 `*`로 표시하며 Save/Ctrl+S와
Save/Discard/Cancel 종료 확인을 제공한다. Library Manager 종료나 Cell/Library
삭제도 관련 Workspace의 닫기 확인이 취소되면 중단한다.

`ManagedSourceService`는 BOM과 LF/CRLF를 보존하고 content hash, manifest
revision, Library writer lease를 다시 확인한 후 staging과 atomic replace로
저장한다. Workspace의 file watcher는 Cell의 `views`를 한 번만 감시한다.
clean 파일은 다시 읽고 dirty 파일은 충돌 상태를 유지하며 사용자에게
Reload/Overwrite/Cancel을 요구한다. 잘못된 UTF-8, NUL 또는 16 MiB 초과 파일은
편집하지 않는다. 외부 링크와 Library 밖 경로는 허용하지 않는다.

worker 이벤트는 session generation을 포함해 닫히거나 재사용된 HWND에 적용되지
않는다. Win32 control은 GUI thread에서만 갱신한다.

Verilator Lint는 저장된 Project만 실행한다. 설정과 경로를 검증하고 Verilator를
probe한 뒤 구조화된 `WslCommand`를 runtime에 넘긴다. CPU 사용량은
cross-process named semaphore quota에 포함한다. 각 실행은
`.designpp/runs/<run-uuid>`에 atomic manifest, raw log, diagnostics를 보존하며
재시작 시 미완료 Running 기록을 Interrupted로 복구한다. Cell 로그는 Workspace와
Library Manager 통합 Output에 함께 전달한다.

Lint 진단은 Problems와 Monaco marker에 동시에 반영된다. Problems를
더블클릭하면 해당 관리 파일을 열고 정확한 줄과 열로 이동한다. Lint 전 dirty
자료가 있으면 Save All을 먼저 완료해야 하며 메모리의 미저장 소스를 임시 입력으로
사용하지 않는다.

활성 Monaco 탭이 Testbench View 소속이면 오른쪽 Inspector는 Icarus 실행
패널로 전환된다. 파일별 Testbench Top과 VCD 설정은 `.dpproj`에 저장한다.
실행은 모든 enabled RTL과 같은 Testbench View의 enabled 파일을 사용해
`iverilog -g2012` 컴파일 후 `vvp`를 순차 실행한다. VCD 사용 시 run 전용 wrapper를
생성하므로 Library 소스는 수정하지 않는다. 성공한 VCD는 Runs/Artifacts에 남고
사용자가 Open Waveform을 선택할 때만 GTKWave를 실행한다. Lint와 Simulation은
동시에 실행하지 않으며 두 단계 전체가 CPU token 하나와 동일한 취소 수명을
공유한다.

`Debug Testbench`는 같은 저장·검증·compile 입력으로 VVP 대화형 세션을 시작한다.
세션은 time 0에서 정지하고 Continue, scheduler event 단위 Step, Finish와 강제
Stop을 제공한다. 하단 Debug 탭은 현재 time/scope, scope item, 선택한 signal 값과
whitelist console transcript를 표시한다. `$stop`에 파일과 줄 정보가 있으면 해당
Monaco model을 열어 위치를 표시한다. Debug run의 정상 또는 partial VCD는 실행이
끝난 뒤 기존 Open Waveform 동작으로 GTKWave에서 연다. 실행 중 임의 Pause,
Monaco breakpoint instrumentation, statement stepping과 persistent Watch는 v1에
포함하지 않는다.

Testbench의 실행 단계는 `TestbenchExecutionService`를 통해 시작한다. 서비스는
한 Workspace의 Simulation/Debug process handle, generation, 취소와 terminal
delivery를 직렬화한다. output과 completion은 immutable event로 Workspace에
전달되며, 종료·취소·중복 completion callback은 Run을 한 번만 종결한다. 취소된
run에서 유효한 VCD는 `partial` artifact로 등록할 수 있고, 빈 파일, reparse
point, run 밖의 경로는 artifact로 등록하지 않는다.

Monaco 자산은 `editor/`에서 esbuild로 로컬 bundle되며 실행 중 CDN을 사용하지
않는다. WebView2는 `https://designpp-editor.example/`만 허용하고 외부 navigation,
popup, permission 및 native host object를 차단한다. WebView2 Runtime이 없거나
초기화가 실패해도 Workspace와 프로젝트/Lint 기록은 종료하지 않는다. Runtime은
Library Manager의 Tool Check에서 버전을 확인하고 선택 설치/복구할 수 있다.
초기화 실패 메시지와 Output에는 원래 HRESULT를 보존한다. Evergreen Runtime은
다른 Windows 앱도 공유하므로 Tool Check의 삭제 대상에는 포함하지 않는다.
