# Design++ architecture

## Dependency direction

```text
app -> gui -> runtime
       |       |
       v       v
      core    wsl.exe -> Linux EDA tools
```

`core` contains project, stage, artifact, and run concepts. It does not include
Windows headers. `runtime` owns process creation, redirected output, cancellation,
and WSL2 command translation. `gui` owns HWNDs and converts worker callbacks into
window messages before touching controls.

## Current GUI ownership

`LibraryManagerWindow` is the process's startup and main window. It owns the
empty library explorer and library list, bottom Output log, global status,
progress, and cancellation command. Mock project and flow rows are not inserted.
Its standard Win32 Tools menu opens one owned
`ToolCheckWindow`; opening the command again activates that existing window.
Tool Check only presents tool state and requests a probe. Runtime output is routed
back to the Library Manager Output pane and is never duplicated into the tool
window.

## WSL2 invocation

Linux programs are represented by `WslCommand`:

- optional distribution
- optional Linux working directory
- executable path or command name
- argument vector
- environment delta

The runtime translates that model into:

```text
wsl.exe [--distribution DISTRO] [--cd DIR] --exec \
  [/usr/bin/env KEY=VALUE ...] PROGRAM ARGUMENT...
```

This path intentionally avoids `bash -c` for ordinary tool invocations. A backend
that genuinely requires shell language must request `/bin/bash -lc` explicitly
and keep the script content inside the adapter.

## Process ownership

Every external run receives a Windows Job Object configured with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. Cancelling a run terminates the complete
`wsl.exe` process tree instead of only the immediate process. Output from stdout
and stderr is combined and streamed from a worker thread.

## Next domain objects

The next implementation slice should add:

1. Toolchain profile and WSL distribution discovery
2. Project persistence using a versioned `.dpproj` format
3. Typed artifacts and run manifests
4. Tool adapter interface and capability probing
5. Verilator lint adapter as the first complete vertical flow
