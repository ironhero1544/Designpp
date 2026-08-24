# Design++ Agent Instructions

## 1. Authority and Required Context

These instructions apply to the entire repository.

Before changing code, read the relevant sections of:

1. `docs/PROJECT_PLAN.md` — product scope, stages, roadmap, and acceptance goals.
2. `docs/ARCHITECTURE.md` — dependency direction and execution boundary.
3. This file — implementation, safety, style, and verification rules.

The current user request is authoritative. If it changes an architectural decision,
update the affected design document in the same change. Do not silently diverge from
`PROJECT_PLAN.md`.

When requirements conflict, use this order:

1. Explicit current user instruction
2. Safety and data-integrity requirements
3. `PROJECT_PLAN.md` and `ARCHITECTURE.md`
4. This file
5. Existing local convention

## 2. Fixed Technology Decisions

- Language: C++20 only; do not use C++23 features.
- GUI: native Win32 API.
- Compiler: MSVC.
- Build: Visual Studio and MSBuild using `Design++.slnx`.
- Primary target: x64 Windows.
- EDA execution: WSL2 through the runtime layer and `wsl.exe`.
- Managed backends: OpenLane 2 and OpenROAD Flow Scripts.
- Embedded source editor: Monaco Editor 0.56.0 hosted by Microsoft WebView2
  1.0.4078.44. Minimal JavaScript ESM under `editor/` is the only exception to
  the C++ implementation-language rule; it must not contain application,
  persistence, runtime, or EDA business logic.
- Monaco assets are bundled by esbuild 0.28.1 at build time and are never loaded
  from a CDN at runtime. TypeScript, React, Electron, and VS Code extensions are
  outside the approved editor boundary.
- Do not add CMake or Qt unless the user explicitly changes the project direction.
- Do not add a third-party dependency without prior user approval.

## 3. Build and Verification Commands

Run commands from the repository root in a Visual Studio Developer PowerShell.

```powershell
msbuild Design++.slnx /m /p:Configuration=Debug /p:Platform=x64
msbuild Design++.slnx /m /p:Configuration=Release /p:Platform=x64
```

Formatting, once `clang-format` is available:

```powershell
clang-format -i --style=Google <changed C++ files>
```

Do not recursively format unrelated files. Generated files and Visual Studio resource
files are exempt unless the generator owns their formatting.

Before finishing a code change:

1. Format every changed C++ file with Google style.
2. Run the relevant unit, contract, integration, and concurrency tests.
3. Build x64 Debug successfully.
4. Build x64 Release for runtime, concurrency, persistence, or packaging changes.
5. Run `git diff --check`.
6. Report any unavailable verification tool explicitly.

## 4. Architecture and Extensibility

Maintain the following dependency direction:

```text
app -> gui -> application services -> core
                         |
                         v
                      runtime -> wsl.exe -> Linux EDA tools
                         |
                         v
                      adapters
```

### 4.1 Layer boundaries

- `core` contains Project, Flow, Stage, Artifact, Run, Status, and Metric concepts.
- `core` must not include Windows headers or depend on HWND, HANDLE, messages, controls,
  WSL2, or a concrete EDA tool.
- `runtime` owns process creation, job objects, output capture, cancellation, WSL2,
  scheduling, path mapping, and cross-process coordination.
- `adapters` translate tool-neutral requests into tool-specific commands and parse
  results. Tool-specific behavior must remain outside `core` and `gui`.
- `gui` owns Win32 controls and presentation only. It calls application services and
  never assembles EDA shell commands.
- `app` performs composition and startup. It must not contain business logic.

### 4.2 Required extension points

Design interfaces around capabilities, not tool names. Keep these concepts separable:

- `ToolAdapter`
- `ManagedFlowAdapter`
- `ExecutionProvider`
- `TaskScheduler`
- `ProjectStore`
- `ArtifactStore`
- `RunStore`
- `PathMapper`
- `EventSink`
- `DiagnosticParser`

Adding a new EDA tool should normally require a new adapter and registration metadata,
not edits to GUI command switches or central `if (tool == ...)` chains.

### 4.3 Compatibility and persistence

- Every persisted format has an explicit schema version.
- Readers validate unknown, missing, and deprecated fields.
- Migrations are explicit, tested, and never overwrite the only copy of user data.
- Tool capability and version probing precedes command generation.
- Artifacts carry input hashes, tool versions, platform/PDK identity, and timing corner.
- Incompatible artifacts must fail validation instead of being silently connected.
- Raw logs and original reports remain available even when normalized parsing fails.

## 5. Google C++ Style Guide — Mandatory

The current official
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) is the
authoritative C++ rule set. Follow it in full, not only its formatting section.

If this summary and the official guide differ, the official guide wins.

### 5.1 Language and files

- Target standard C++20 and use no non-standard C++ language extensions.
- New headers use `.h`; new implementation files use `.cc`.
- New file names are lowercase `snake_case`.
- Headers are self-contained and include what they use.
- Use project-path include guards such as
  `DESIGNPP_RUNTIME_TASK_SCHEDULER_H_`; do not use `#pragma once` in new code.
- Include order is: related header, C system/Windows headers, C++ standard library,
  third-party headers, then project headers. Separate groups with one blank line and
  sort within a group.
- Include Windows SDK headers with angle brackets and their canonical spelling, for
  example `<windows.h>`.
- Source files are UTF-8. User-facing localized text belongs in resources rather than
  hard-coded C++ strings when practical.
- `wchar_t` is allowed only at the Windows API boundary. Core text is UTF-8.

The existing `.hpp`, `.cpp`, `#pragma once`, hard-coded UI text, and pre-guide naming
are migration debt. Do not copy those patterns. When touching an affected component,
bring the complete touched file into compliance. Plan a dedicated migration before
substantial feature development.

### 5.2 Naming

- Types, classes, structs, aliases, and concepts: `PascalCase`.
- Functions and methods: `PascalCase`.
- Variables and parameters: `snake_case`.
- Data members: `snake_case_` with a trailing underscore.
- Constants: `kPascalCase`.
- Enumerators: `kPascalCase`.
- Namespaces: lowercase `snake_case`.
- Macros: uppercase with a project-specific prefix; avoid macros when possible.
- Names must describe intent. Do not use unclear abbreviations or encode types in names.

Win32 SDK callback types and externally required ABI names may follow the SDK spelling.
Keep the exception at the smallest boundary and wrap it with project-style code.

### 5.3 Formatting and comments

- Use Google `clang-format` style with a normal 80-column limit.
- Indent two spaces; never use tabs for C++ indentation.
- Use braces and line wrapping exactly as produced by Google style.
- Comments explain intent, invariants, ownership, concurrency, or non-obvious choices.
- Public APIs receive Google-style declaration comments when behavior, ownership,
  thread safety, failure, or lifetime is not obvious.
- Do not add repetitive comments that merely restate a name.
- Code identifiers and engineering comments are English. UI resources may be Korean.
- Use inclusive language.
- Every new C++ file must use the repository's approved license header. If no license
  has been approved yet, do not invent legal text; request the project owner's decision.

### 5.4 C++ usage

- Do not write `throw`, `try`, `catch`, `std::exception_ptr`, or
  `std::nested_exception` in project code.
- Recoverable failure uses the project `Status`/`Result<T>` abstraction once provided.
  Until then, use an explicit status enum/result struct rather than exceptions.
- Constructors establish valid objects. Use factories when construction can fail.
- Avoid RTTI and `dynamic_cast`; prefer virtual interfaces, variants, or explicit
  capability data.
- Follow RAII. Never use owning raw `new` or `delete`.
- Use `std::unique_ptr` for single ownership. Use `std::shared_ptr` only when shared
  lifetime is real and documented; prevent reference cycles.
- Raw pointers and references are non-owning unless an API explicitly documents
  otherwise.
- Avoid global mutable state and non-trivially destructible namespace-scope objects.
- Do not use `using namespace`.
- Use scoped enums.
- Use `override`, `const`, `constexpr`, and `[[nodiscard]]` where semantically correct.
- Use `noexcept` only when it is accurate and useful under the Google guide.
- Prefer clear code over clever metaprogramming or speculative abstraction.
- Do not add a generic abstraction until at least two concrete use cases justify it,
  except for boundaries already required by `PROJECT_PLAN.md`.

### 5.5 Win32 exceptions to portability rules

The following are permitted only inside Win32 boundary code:

- Windows SDK types, calling conventions, and callback macros.
- UTF-16 and `wchar_t` required by wide Win32 APIs.
- SDK-generated resource identifiers and generated resource files.
- Compiler/linker configuration required to declare a Windows manifest or ABI.

Do not let those exceptions leak into `core`. Prefer manifest files and standard build
settings over source-level compiler pragmas when both options are available.

## 6. Multicore and Resource-Aware Execution

Design++ must use available CPU cores without oversubscribing the machine or freezing
the GUI. Multicore support is an architectural requirement, not an optional later
optimization.

### 6.1 Scheduler model

- Implement a bounded, resource-aware scheduler; never create one unbounded thread per
  stage, log stream, file, or callback.
- Use `std::jthread`, `std::stop_token`, condition variables, and RAII-owned workers for
  native worker pools.
- Do not use detached threads.
- Do not rely on `std::async` scheduling behavior for core execution.
- Determine the default logical CPU budget from `std::thread::hardware_concurrency()`.
- If it returns zero, use a safe fallback of one worker.
- By default reserve at least one logical processor for Windows and UI responsiveness:
  `max(1, logical_cpu_count - 1)` execution tokens.
- Allow the user to override the CPU budget within validated limits.
- Each task declares requested CPU tokens, memory estimate, exclusivity requirements,
  and cancellation support.
- A task starts only after acquiring its resource tokens. Always release tokens through
  RAII, including failure and cancellation paths.
- Independent DAG stages may run concurrently. Stages with data dependencies, shared
  mutable outputs, or backend restrictions must remain serialized.
- Use fairness so a large task cannot permanently starve and small tasks cannot
  permanently block a reserved full-flow task.

### 6.2 External EDA tools

- The scheduler accounts for cores used by external WSL2 tools, not only native C++
  threads.
- Adapters expose supported parallelism and generate native tool flags only when the
  detected version supports them.
- Do not blindly pass the machine's full core count to every simultaneous tool.
- ORFS/OpenLane/OpenROAD ownership of internal parallelism must be respected.
- Multiple processes share a cross-process CPU quota through a Windows named semaphore
  or an equivalently robust coordinator.
- Mutations of a shared toolchain, package cache, or PDK cache require a separate named
  cross-process lock and must not run alongside readers unless the operation is proven
  safe.

### 6.3 Concurrency correctness

- Prefer immutable data and message passing over shared mutable state.
- Protect every shared invariant with one clearly owned synchronization strategy.
- Atomics are for simple independent state. Do not spread a compound invariant across
  unrelated atomics.
- Document lock ordering for code that can acquire more than one lock.
- Never call user callbacks, parser plugins, Win32 `SendMessage`, or blocking I/O while
  holding scheduler or store locks.
- Every blocking wait must have cancellation and shutdown behavior.
- Cancellation is cooperative first and forceful process-tree termination second.
- Completion is delivered exactly once.
- Tests must exercise start/cancel/finish races, repeated shutdown, high task counts,
  and simultaneous process output.

## 7. GUI Threading and Multiple Instances

Design++ must support multiple top-level windows in one process and multiple Design++
processes on the same Windows user session.

### 7.1 Per-window isolation

- No global `current_project`, `main_window`, selected stage, output HWND, or active run.
- Each top-level window owns an independent controller/session and project context.
- All HWND access occurs on the thread that created the window.
- Worker threads communicate through immutable events posted to a live window/session.
- Never update a Win32 control directly from a worker thread.
- Posted payloads have explicit ownership and are reclaimed if posting fails, the window
  closes, or queued messages are discarded.
- Prevent HWND reuse bugs with a window/session generation token or equivalent lifetime
  validation.
- On close, stop accepting events, request cancellation, drain or invalidate posted
  work safely, then destroy controls and owned state.
- Window class registration must be idempotent and safe when several windows are created.

### 7.2 Multi-process isolation

- Do not enforce a single application instance unless the user explicitly requests it.
- Never use fixed temporary file, pipe, event, mutex, shared-memory, log, or run names.
- Include a stable project identity plus a UUID where uniqueness is required.
- Every run gets an exclusively created run directory; timestamp alone is insufficient.
- Do not change the process-wide current directory or process-wide environment after
  startup. Set working directories and environments per child process.
- Project writes use a cross-process writer lease and atomic replace.
- If another process owns the project writer lease, open the project read-only or ask the
  user before taking ownership. Never allow silent last-writer-wins corruption.
- A writer lease records process identity and start information so stale locks can be
  recovered safely after verifying that the owner no longer exists.
- User settings and recent-project data use atomic writes or a supported transactional
  Windows store.
- Shared caches use content-addressed entries or locks; partially written entries are
  never visible as valid.
- A crash in one GUI process must not invalidate another process's run directory or
  release resources it does not own.

### 7.3 UI responsiveness

- The Win32 message loop performs no EDA execution, long parsing, recursive scanning,
  network access, WSL startup, blocking waits, or large file I/O.
- Long work runs through the scheduler and publishes progress events.
- Throttle/coalesce high-volume log and progress updates before posting them to the UI
  queue so the message loop cannot be flooded.
- Bound in-memory log buffering and spill large logs to the run store.
- Closing a window must remain responsive even when WSL2 or an EDA tool is unresponsive.

## 8. WSL2 and External Process Rules

- GUI and `core` never construct shell command strings.
- Use a structured request containing distribution, Linux working directory, program,
  argument vector, and environment delta.
- Ordinary commands use `wsl.exe --exec` without a shell.
- `/bin/bash -lc` is allowed only when shell language is genuinely required and the
  owning adapter encapsulates and tests all quoting.
- Never interpolate untrusted paths or user values into shell source.
- Capture stdout and stderr without blocking either stream.
- Preserve raw bytes/logs and decode using an adapter-declared encoding. Linux tool
  output defaults to UTF-8; Windows-host `wsl.exe` administrative output may be UTF-16.
- Assign every external run to an independent Job Object with process-tree cleanup.
- Record executable, arguments with secrets redacted, tool version, distribution,
  working directory, environment delta, start/end time, exit code, and cancellation.
- Treat process exit code, artifact validation, and stage success as distinct results.
- Never expose credentials, license tokens, private PDK paths, or sensitive environment
  values in logs.

## 9. Errors, Diagnostics, and Observability

- Do not use user-visible strings as machine-readable error identities.
- Errors carry a stable code, category, stage/run identity, operation, technical detail,
  and user-facing message.
- Preserve the original Windows error code or Linux exit/signal status.
- Add context at layer boundaries; do not discard the underlying cause.
- Expected user/configuration errors must not crash the process or trigger assertions.
- Assertions are for programmer invariants, not recoverable runtime failures.
- Logs identify process, window/session, project, run, stage, and task where applicable.
- Concurrent event ordering uses a monotonic per-run sequence number.
- Repeated high-volume diagnostics are rate-limited without hiding the raw log.

## 10. Testing Requirements

New behavior is not complete without tests at the appropriate levels.

### 10.1 Unit tests

- Project and flow validation
- Path mapping and command argument generation
- Status/result propagation
- Scheduler token accounting and fairness
- Artifact compatibility and stale detection
- Log and metric parsers

### 10.2 Contract tests

Use controlled fake executables to test:

- stdout/stderr streaming
- nonzero exit codes
- missing executables
- cancellation before start, during start, and during output
- child-process tree termination
- malformed and very large output
- callback delivery exactly once

### 10.3 Concurrency and multi-instance tests

- Multiple top-level windows with independent projects
- Two or more processes opening the same project
- Writer/read-only lease behavior and stale lease recovery
- Unique run directory creation under contention
- Cross-process CPU quota enforcement
- Simultaneous log streams and window closure
- Repeated scheduler startup/shutdown
- Crash/restart recovery without corrupting manifests
- Stress runs at and above the logical CPU count

Tests must be deterministic. Do not use arbitrary sleeps as synchronization when an
event, barrier, latch, condition variable, or observable state can be used.

## 11. Change Workflow

For each implementation task:

1. Identify the owning layer and relevant roadmap phase.
2. Inspect existing changes and preserve unrelated user work.
3. Define or extend the smallest stable interface at the boundary.
4. State ownership, thread affinity, cancellation, and failure behavior before coding.
5. Implement the smallest end-to-end vertical slice.
6. Add unit and race/contract coverage proportional to risk.
7. Format with Google style and audit the touched files against the full guide.
8. Build and run relevant tests.
9. Update `PROJECT_PLAN.md` or `ARCHITECTURE.md` if the design changed.
10. Report verification and remaining limitations precisely.

## 12. Completion Gate

A change is complete only when all applicable statements are true:

- It follows the official Google C++ Style Guide.
- It respects the documented layer boundaries.
- It does not block the GUI thread.
- It has bounded resource usage.
- It has defined cancellation and shutdown behavior.
- It remains correct with multiple windows and multiple processes.
- Persistent writes are atomic and concurrency-safe.
- Errors retain actionable context.
- Relevant tests pass.
- x64 Debug builds successfully.
- Required documentation matches the implementation.

## 13. Prohibited Practices

- Reintroducing CMake or Qt without explicit approval.
- Adding dependencies without explicit approval.
- Running Linux EDA tools directly from GUI code.
- Building commands through unsafe string concatenation.
- Detached or unbounded threads.
- Blocking the Win32 message loop.
- Mutable global application/project/run state.
- Fixed temporary or run names.
- Silent last-writer-wins project persistence.
- Sharing a writable run directory between processes.
- Holding locks while invoking callbacks or blocking external operations.
- Assuming all CPU cores belong to one Design++ process.
- Assuming only one GUI window or process exists.
- Parsing only normalized summaries while discarding raw logs.
- Editing generated build output directories.
- Declaring work complete without relevant build and test evidence.
