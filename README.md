# Design++

Design++ is a native Win32 EDA flow orchestrator. It presents RTL verification,
synthesis, timing analysis, physical design, sign-off checks, and final artifacts
through one Windows desktop application while running Linux EDA tools in WSL2.

The consolidated product scope and implementation roadmap are documented in
[docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md).

## Current foundation

- Library Manager as the native Win32 startup and main window
- Left library explorer, empty library list, and bottom Output panes
- Independent Tool Check window opened from the standard Tools menu
- Per-tool install/update and dependency-aware removal actions
- One global Library Manager log for tool checks and setup operations
- C++20 flow and project domain models
- Asynchronous Windows process runner
- Job Object based process-tree cancellation
- Structured `wsl.exe --exec` command construction
- Library Manager with parallel tool/version probes
- Confirmed WSL2/Ubuntu setup using an elevated child process, without relaunching
  the GUI
- Provider-based APT, cocotb, Nix/OpenLane 2, and ORFS installation/update
- Per-Monitor V2 high-DPI support for the main and owned tool windows

See [docs/LIBRARY_MANAGER.md](docs/LIBRARY_MANAGER.md) for setup behavior,
installed packages, safety boundaries, and current limitations.

## Repository layout

```text
include/designpp/
  core/       Project and flow models
  runtime/    Process and WSL2 APIs
  gui/        Native Win32 window APIs

src/
  app/        Windows entry point
  core/       Platform-independent domain implementation
  runtime/    Win32 process and WSL2 implementation
  gui/        Main window and native controls

docs/         Architecture and implementation notes
```

## Build

Open `Design++.slnx` in Visual Studio, or run from a Visual Studio Developer
PowerShell:

```powershell
msbuild Design++.slnx /m /p:Configuration=Debug /p:Platform=x64
```

The application itself is a Windows executable. WSL2 and a configured Linux
distribution are required before EDA adapters can run. Library Manager can
bootstrap WSL2 and Ubuntu; Windows may require administrator approval and a
restart during first-time setup.

## Execution boundary

GUI code never launches Yosys, Verilator, OpenROAD, or other EDA tools directly.
It submits a structured Linux command to the runtime layer, which starts
`wsl.exe`, captures output asynchronously, and reports completion back to the UI.

OpenLane 2 and OpenROAD Flow Scripts will be implemented as separate managed
flow adapters. Standalone Verilator, Icarus, cocotb, Yosys, and OpenSTA adapters
will use the same runtime layer.
