# Design++ architecture

## Dependency direction

```text
app -> gui -> application services -> core
                         |
                         +-> persistence
                         |
                         v
                      runtime -> wsl.exe -> Linux EDA tools
```

`core` contains project, stage, artifact, and run concepts. It does not include
Windows headers. `runtime` owns process creation, redirected output, cancellation,
and WSL2 command translation. `gui` owns HWNDs and converts worker callbacks into
window messages before touching controls.

`LibraryService` owns Library/Cell/View mutations. `LibraryStore` owns the
versioned UTF-8 JSON `.dplib` codec, revision checks, and atomic manifest replace.
The GUI submits these operations to a bounded `TaskScheduler`; immutable results
return through the per-window event channel. `LibraryBrowserModel` filters an
immutable Library snapshot for the independent Library, Cell, and View panes.
Each request has a generation number, so an older result cannot replace a newer
query after fast typing or parent selection changes.

Each Library uses a file-backed exclusive writer lease. Multiple processes may
read the same Library, but a mutation must hold the lease and match the manifest
revision. Paths stored in the manifest are Library-relative and validated before
filesystem access.

`WorkspaceRegistry` owns top-level View window lifetime and uniqueness through
the tool-neutral `ViewWindow` interface. `ViewWindowFactory` maps a View kind to
one concrete top-level class: source views use `VerilogWindow`, while synthesis
views use `SynthesisWindow`. A tool window never embeds another tool's controls;
in particular, `SynthesisWindow` does not create Monaco and `VerilogWindow` does
not create synthesis controls. Future timing, layout, and report windows extend
the same factory boundary instead of adding command branches to Library Manager.

A View session holds the project snapshot, dirty state, cancellation handle,
writer lease, and session generation. `ProjectService` resolves `.dplib` managed
files into an immutable Source Set; `ProjectStore` validates and atomically
persists versioned `.dpproj` data. Neither service depends on HWND. `PathMapper`
is the sole Windows-to-WSL path translation boundary.

`ManagedSourceService` is the application boundary for editable Library files.
It validates managed relative paths and UTF-8, preserves BOM and line endings,
checks the expected content hash, acquires the Library writer lease, and updates
the source plus `.dplib` as one recoverable operation. `FileWatchService` owns a
single recursive directory watcher per Workspace and sends coalesced immutable
path batches to the Workspace event channel; it never calls Win32 controls.

The embedded editor is a presentation boundary. `MonacoEditorHost` owns one
WebView2 controller per `VerilogWindow` and communicates with the local Monaco
asset by a versioned, size-bounded JSON protocol. `WorkspaceRegistry` shares the
WebView2 environment between Verilog windows, while each Design++ process uses an
independent UUID user-data directory. JavaScript never receives Windows paths,
filesystem access, host objects, EDA commands, or persistence responsibilities.
The only allowed origin is the reserved special-use virtual host
`https://designpp-editor.example/`;
navigation, popups, permissions, and external network content are denied.

`ToolAdapter` validates a project and produces a structured command without
starting a process. `VerilatorAdapter` owns Verilator arguments, capability
probing, and diagnostic parsing. `RunStore` owns UUID run directories, atomic
state manifests, raw logs, and normalized diagnostics. The runtime
`ResourceCoordinator` holds cross-process CPU semaphore tokens for the complete
lifetime of an external run.

## Current GUI ownership

`LibraryManagerWindow` is the process's startup and main window. It owns three
side-by-side owner-data ListViews for Library, Cell, and View, their independent
search and filters, the bottom Output log, global status,
progress, and cancellation command. Mock project and flow rows are not inserted.
Its standard Win32 Tools menu opens one owned
`ToolCheckWindow`; opening the command again activates that existing window.
Tool Check only presents tool state and requests a probe. Runtime output is routed
back to the Library Manager Output pane and is never duplicated into the tool
window. The tool catalog stores platform-neutral `ProcessRequest` probes, so the
same bounded process runner can inspect Windows-host requirements such as the
shared WebView2 Evergreen Runtime and WSL-host EDA tools. Tool Check can request
an elevated WebView2 install/repair, but removal never uninstalls this shared
Windows component.

The app initializes one Win32 single-threaded COM apartment before creating GUI
windows. WebView2 environments and controllers are created on that apartment;
failure diagnostics preserve the originating HRESULT for Tool Check and support
triage.

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

When no Linux working directory is supplied, `WslExecutor` explicitly starts the
command in the distribution user's home directory. `ProcessRunner` gives every
non-interactive child a valid EOF-producing standard-input handle; it never
inherits the absent console handle of the Win32 GUI process.

## Process ownership

Every external run receives a Windows Job Object configured with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. Cancelling a run terminates the complete
`wsl.exe` process tree instead of only the immediate process. Output from stdout
and stderr is combined and streamed from a worker thread. A child is created
suspended, assigned to its configured Job Object, and only then resumed. Failure
to create or configure the Job Object, assign the child, or resume it is reported
as a launch failure; the child is never allowed to continue unmanaged.

## Implemented vertical slice

View open request → Cell Workspace → Project load/source resolution → Verilator
probe/lint → raw log and diagnostics persistence forms the first complete vertical
slice. Toolchain profile and WSL distribution selection, typed generated artifacts,
and additional adapters remain subsequent extensions.

The next completed presentation slice is Source double-click → managed UTF-8 load
→ Monaco model/tab → explicit atomic save → external-change conflict handling.
Verilator diagnostics are mapped to Monaco markers and Problems navigation without
moving parsing or file ownership into the GUI or JavaScript layers.

The Workspace Source Tree is presentation-only: it groups the immutable Source Set
by View kind, View, and relative folder. Add Files requests reuse `LibraryService`
on the bounded scheduler, then atomically replace the Workspace Library snapshot and
resolved Source Set on the GUI thread. The Library Manager is asked to rescan after a
Workspace mutation so other Cell windows and browser panes do not retain a stale
manifest revision.

`SimulationAdapter` is the capability boundary for multi-step simulation plans.
`IcarusSimulationAdapter` validates the active Testbench View and produces separate
structured `iverilog` and `vvp` commands without shell composition. The Workspace
owns the asynchronous step state and one cross-process CPU token; immutable
generation-tagged events advance probe, preparation, compile, execute, and artifact
validation. Run manifest schema v2 records run-relative waveform artifacts while the
reader remains compatible with existing schema v1 records.

Interactive debug execution extends the runtime process boundary with an opt-in,
bounded stdin queue. `ProcessSession::WriteInput` only enqueues UTF-8 bytes; a
RAII-owned writer thread performs pipe I/O so the GUI thread never blocks.
`IcarusDebugAdapter` produces `vvp -i -s` commands and parses prompt-delimited
events, while `DebugSessionService` owns the tool-neutral state transition from
probe and compile through paused/running/completed states. Only whitelisted VVP
commands cross this boundary. Debug runs retain the same CPU lease, cancellation,
raw-log, diagnostic, and run-relative artifact guarantees as Simulation runs.

`ExecutionProvider` is the application-facing external-process boundary. Its
WSL implementation owns a cancellable `ProcessSession`, while test providers can
deterministically publish output, launch errors, completion, and duplicate
completion callbacks. `TestbenchExecutionService` serializes one Verilog window
simulation or debug process, rejects stale generation callbacks, and emits at
most one terminal event. It owns no HWND and posts immutable events through its
caller-provided sink. `VerilogWindow` remains responsible for user prompts,
control presentation, and Monaco navigation.

`SynthesisRunService` owns the complete Yosys state machine: capability probe,
CPU-token acquisition, exclusive run-directory creation, script persistence,
structured execution, raw-log preservation, artifact validation, statistics
parsing, cancellation, and exactly-once terminal delivery. `SynthesisWindow`
only loads presentation snapshots and marshals immutable service events through
its generation-checked window channel. It never constructs WSL commands or
parses reports on the GUI thread.

`YosysAdapter` also normalizes the tool-specific JSON netlist into an immutable
gate schematic model containing top-level ports, cells, pins, and net bits.
Parsing remains off the GUI thread. `GateSchematicCanvas` is a native Win32
presentation component that lays out and paints this model with scrollable GDI
graphics. Known primitive cells use vector logic symbols, while unknown or
technology-specific cells use labeled generic symbols. Connections are grouped
by net into shared routing trunks and explicit junctions. The canvas never reads
artifacts or understands the Yosys JSON format. Primitive symbols consistently
use the IEEE 91 distinctive-shape family rather than mixing it with uniform
rectangular symbols. Cursor-centered zoom, fit-to-window, scrollbars, and
middle-button panning operate entirely on the immutable presentation model.
Cells are assigned columns from resolved net dependency depth and ordered
vertically from upstream port order, so combinational paths progress from inputs
to outputs instead of following arbitrary JSON cell enumeration.
Repeated forward and backward barycenter sweeps order the cells inside each
level using both upstream and downstream connectivity, reducing edge crossings
before routing begins.
All cells at the same logic depth share one fixed X coordinate and occupy unique
Y rows in a vertically centered level column; a logic level is never spread
horizontally. Column and row spacing use schematic-grid multiples to reduce
ambiguous overlapping runs between adjacent levels.
Top-level input terminals are vertically aligned near the average position of
their sink pins, while outputs align with their actual driver. Port collision
resolution preserves a minimum vertical gap.
The canvas paints a zoom-aware line grid below all nets and symbols. Minor lines
follow the router's eight-unit pitch, while every fifth line is darker and
thicker. The grid follows the same logical origin during scrolling and panning
and reduces minor-line density at low zoom levels.
The schematic palette is semantic and intentionally sparse: symbols, wires,
pin lines, and text are monochrome. Only pin terminals are red filled squares,
and actual three-or-more-direction branches of the same net are blue filled
circles. Straight segments, terminal endpoints, and two-segment bends do not
receive a junction marker. Decorative cell and port fill colors are not used.
Nets and pin stubs are painted before the opaque symbol body to prevent routing
lines from visually crossing a gate outline.
Junction detection also consults net topology: a source net with multiple sink
ports always receives at least one visible blue fan-out marker even when aligned
sinks collapse the routed geometry into coincident or endpoint segments.
`OrthogonalEdgeRouter` owns schematic edge routing independently from the Win32
canvas. It emits horizontal/vertical polylines, topology-derived junctions, and
non-connecting crossing bridges; the canvas only paints that immutable result.
Adjacent and multi-level connections use the same deterministic cost framework:
wire length, bend count, occupied-track overlap, near-parallel clearance, and
different-net crossings are evaluated together. Both horizontal trunks and
vertical entries are checked against expanded cell bounds. A blocked vertical
entry may use a short orthogonal dogleg in a neighboring lane instead of passing
through a symbol. Multi-sink nets start from the median sink height and share a
pseudo-Steiner horizontal trunk before branching to individual sinks. Junctions
are derived from final same-net topology only; a different-net crossing receives
a small monochrome bridge after routing and never a blue connectivity marker.
Track and dogleg searches use a bounded set of pin, obstacle-boundary, and
occupied-track coordinates instead of scanning every canvas grid coordinate.
The canvas caches the immutable routing result for the current schematic, so
zooming, panning, exposing, and repainting never execute the router again.
Two-input primitive symbols use a compact body ratio of approximately 1.2:1.
An XOR input stub terminates at the additional rear curve rather than crossing
it to reach the OR-shaped body curve.
The gate function/type label is centered below each symbol. A compact `U1`,
`U2`, and so on instance reference is placed above the symbol at the right.
The normalized model continues to retain the complete original Yosys instance
name even though it is not rendered on the compact canvas.
Multi-bit top-level ports are expanded into individually labeled bit terminals
instead of collapsing every bit onto one visual endpoint. Yosys `ANDNOT` and
`ORNOT` primitives retain their inverted B input in the rendered symbol.

Yosys writes its script, netlists, statistics, and report in a per-run
ASCII-safe directory below the Windows temporary directory. On completion the
service copies every available artifact with Windows filesystem APIs into the
real run directory before validation and parsing. This keeps Libraries with
spaces or non-ASCII names supported without exposing a shared or fixed staging
path; each staging directory is keyed by the run UUID. The Yosys command uses
that directory as its working directory, and all generated filenames and the
`-s` script argument are relative. This avoids Yosys `tee -o` failures when it
opens absolute DrvFs paths even when the underlying path is ASCII.

Schema v3 extends each Testbench configuration with a simulator backend,
HDL/cocotb runner, optional cocotb module/filter, and `none`/VCD/FST waveform
format. Schema v1 and v2 are migrated in memory without overwriting the source
file. `VerilatorSimulationAdapter` produces a structured `--binary --timing`
build and a run-scoped executable. Verilator build products may be staged in an
ASCII-safe per-run temporary directory because its generated Makefiles cannot
reliably handle all library paths; validated waveform artifacts are then copied
into the UUID run directory. `CocotbRunnerAdapter` uses
the installed cocotb Makefile entrypoint through structured arguments and
per-process environment deltas; it does not add Python application logic to the
repository. The bounded cocotb xUnit parser keeps `results.xml` authoritative
when normalization fails.

`YosysAdapter` validates enabled RTL and verifies that the configured top module
is declared before producing a run-scoped `.ys` script. The script emits Verilog
and JSON netlists, machine-readable statistics, and a human report.
Without a Liberty library, synthesis lowers inferred combinational selection
logic through ABC into AND/OR/XOR/XNOR/NAND/NOR/NOT primitives instead of
preserving RTL-level selection cells in the final gate artifact. With Liberty
input, ABC maps against the selected technology library instead.
`OpenStaAdapter` requires an explicit Yosys netlist, Liberty set, SDC, and corner,
then produces a run-scoped Tcl script. Marker-delimited raw output is normalized
into WNS, TNS, and timing violations while the original report remains
available. Neither adapter starts processes or constructs commands in the GUI.
