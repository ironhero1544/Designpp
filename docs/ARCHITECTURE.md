# Design++ architecture

## Layout Setup editing and persistence

`LayoutSetupController` owns the heap-allocated per-window editing session.
`LayoutSetupDraft` normalizes blank fields before core validation; adapters and
fingerprints consume effective configuration. Setup and Monaco use the application
message loop without stack-owned state or nested modal loops. Only one editing
surface is enabled while JSON is open. Apply updates the draft; Save and JSON
Ctrl+S use the same asynchronous Cell-scoped ProjectService write. Successful Save
keeps Setup open. Failed writes preserve input. Pending Close is deferred until
save completion, and Run consumes the saved configuration.

## Dependency direction

### Tool Check migration status

Toolchain profile schema v2 preserves legacy paths as custom/unverified and
stores independent OpenLane/ORFS bundle selections. Reading v1 does not rewrite
the settings file. Tool Check's legacy managed-root inventory is read-only:
missing roots and present-but-unprepared roots are distinct, and neither starts
Nix. Tool-specific version parsing rejects progress lines and unknown versions.
The shared inventory/preparation service, resolved environment activation and
rollback integration remain pending; root presence is not a compatibility pass.

The ORFS flake execution environment selects both Yosys and OpenROAD from
the checkout's submodules, without rewriting the installed flake lock.
Install validation and run probing require OpenROAD `repair_timing -sequence`
in addition to Yosys/ABC capabilities. Executable presence or `-version` alone
does not establish ORFS compatibility. Failure must precede the READY marker.

ORFS input preparation creates one immutable `PreparedPhysicalInputs` snapshot
from the actual RTL, include files, defines, parameters, staged SDC, active
backend configuration, and tool identity. Staging, generated configuration,
fingerprinting, and checkpoint selection consume that same snapshot. The input
contract is versioned independently from the project schema; lineages without
the current contract are stale. Automatic resume is advisory: if any prepared
input no longer matches, Generate/Update Layout keeps the old lineage and starts
a new validated lineage. An explicit Rebuild From request rejects an
incompatible parent because copying its prerequisites would mix different design
inputs. Inactive OpenLane-only PDK, SCL, PDN, and I/O values remain persisted but
do not participate in an ORFS fingerprint.

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

Library manifest schema v3 owns shared managed files at Library scope.
Technology Liberty files imported there are resolved for every Cell, while
Cell-specific RTL, testbench, and SDC files remain owned by Views. Library-level
and View-level imports use the same atomic manifest and writer-lease contract.
The presentation boundary follows the same ownership: `ConstraintsWindow` edits
only Cell-scoped SDC files, while `LibertyWindow` manages Library-scoped
`.lib`/`.liberty` files through add, atomic replacement, and recoverable removal.
It also edits managed Liberty text through the same content-hash, writer-lease,
and recoverable-save contract used by other managed source. Synthesis and Timing
windows select those shared Liberty inputs without moving them into a Constraints
View. Library Manager exposes the Liberty manager for both empty and populated
Libraries so the first shared technology file can be added without creating a
placeholder View.

`ManagedSourceService` is the application boundary for editable Library files.
It validates managed relative paths and UTF-8, preserves BOM and line endings,
checks the expected content hash, acquires the Library writer lease, and updates
the source plus `.dplib` as one recoverable operation. `FileWatchService` owns a
single recursive directory watcher per Workspace and sends coalesced immutable
path batches to the Workspace event channel; it never calls Win32 controls.

The embedded editor is a presentation boundary. `MonacoEditorHost` owns one
WebView2 controller per editor surface and communicates with the local Monaco
asset by a versioned, size-bounded JSON protocol. `WorkspaceRegistry` shares the
WebView2 environment between Verilog, Constraints, Liberty, and transient Layout
JSON editors, while each Design++ process uses an independent UUID user-data
directory. JavaScript never receives Windows paths, filesystem access, host
objects, EDA commands, or persistence responsibilities.
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
- optional distribution user for narrowly scoped administrative commands
- optional Linux working directory
- executable path or command name
- argument vector
- environment delta

The runtime translates that model into:

```text
wsl.exe [--distribution DISTRO] [--user USER] [--cd DIR] --exec \
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

Raw process output remains byte-exact in the RunStore. Presentation consumers
join an incomplete trailing UTF-8 code point with the next output chunk before
converting text for Win32 controls because pipe reads are not character
boundaries.

## Implemented vertical slice

View open request → Cell Workspace → Project load/source resolution → Verilator
probe/lint → raw log and diagnostics persistence forms the first complete vertical
slice. Toolchain profiles persist WSL distribution, managed-flow roots, PDK root,
and CPU budget. The Doctor discovers installed distributions and their WSL version
asynchronously before validating the selected profile and tool roots.
The profile store is intentionally per-user because it describes installed tools;
when a project contains `toolchain_profile_id`, Layout binds that cell to the
matching profile and reports a missing profile instead of silently falling back to
another cell's selection. Physical implementation values remain in the cell's
`project.dpproj`.

ToolchainSettings schema v3 separates immutable installed-environment records
from the profile's active and rollback references. `ToolchainInventoryService`
resolves that snapshot for Tool Check, Doctor, flow, verification, and viewers;
`ToolchainPreparationService` only adopts an already prepared candidate or
atomically swaps to a retained rollback environment. Inventory and ordinary
runs never realize a Nix environment, update Git, or delete an installation.

`PhysicalVerificationService` is independent from physical implementation. It
pins a source Layout Run, final GDS, same-run schematic, recipe hash, and
toolchain environment fingerprint before starting a new verification Run.
`PhysicalVerificationAdapter` owns KLayout/Magic/Netgen command and report
contracts. Process success and verification pass remain separate, empty or
malformed reports never pass, and the raw report plus input hash manifest are
retained. Layout only renders immutable events and marks results from another
GDS source Run stale.

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

The Library Manager stores recent Workspace identities as one atomically replaced
per-user registry value protected by a named writer mutex. Entries contain stable
Library/Cell/View IDs plus display names, are deduplicated on successful open, and
are pruned against refreshed Library snapshots before dynamic menu activation.

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

Hierarchical schematic navigation uses module boundaries retained in Yosys JSON
artifacts when synthesis flattening is disabled. Double-clicking a module node
loads that module from the selected run artifact on the bounded worker
scheduler. Parsed models and immutable scenes are cached per window and per
Readable/Gate mode; generation checks reject stale loads while the canvas only
performs transformed hit testing and rendering on the GUI thread.

Schematic layout selects a bounded fast-routing policy for large scenes while
retaining the precise congestion-aware router for smaller scenes. Fast routing
still checks symbol obstacles and same-net junction topology, but limits track
and dogleg candidates and skips the quadratic all-net crossing analysis. Router
loops observe the scene-build cancellation callback so switching views or
closing a window does not leave a large layout consuming CPU.

Yosys preserves two schematic inputs for each new run. The pre-ABC
`schematic-structural.json` retains inferred registers, muxes, comparators, and
arithmetic operators for the default Readable view. The post-ABC `netlist.json`
remains the authoritative Gate view and the source for physical downstream
artifacts. Failure to create or parse the optional structural artifact never
changes an otherwise successful synthesis result; old runs simply expose Gate.

`YosysAdapter` normalizes either JSON file into the tool-neutral immutable
`SchematicModel`. It preserves net names, bit ordering, offsets, parameters,
attributes, pin roles, and sequential polarity. Unknown cells remain labeled
generic nodes rather than being guessed as primitive gates.

`SchematicBuildService` owns parse-independent normalization, validation,
layout, and routing on a bounded worker with CPU-token accounting,
cancellation, generation checking, and exactly-once completion. Registers are
data-depth boundaries, clock/reset/set/enable pins are excluded from data depth,
and unresolved combinational cycles are condensed with SCC diagnostics. In the
Readable view, compatible bit registers sharing named vector Q bits and control
polarity become a register bank. Feedback uses dedicated outer lanes. Gate
scenes over 2,000 nodes or 8,000 connections fail visibly instead of rendering
a misleading subset.

`OrthogonalEdgeRouter` produces deterministic horizontal/vertical polylines,
shared trunks, topology-derived same-net junctions, and different-net crossing
bridges. Trunks terminate at real attachment points. The immutable
`SchematicScene` also records bus widths and normalized summary metrics.

`GateSchematicCanvas` receives only a complete immutable scene and performs GDI
painting. It never reads JSON, analyzes graphs, lays out nodes, or runs the
router. Repaint, zoom, pan, and Fit therefore reuse the same scene. Viewport
and Fit calculations are isolated in a Win32-independent helper;
painting culls off-screen geometry and applies zoom-based detail levels.
Primitive cells use IEEE 91 distinctive shapes; inferred operators and register
banks use
labeled functional symbols. The palette remains semantic: black geometry and
text, red square pin terminals, and blue round three-or-more-direction branches.
Bus wires are thicker and carry slash/width notation, while single-bit wires
remain one pixel. A zoom-aware eight-unit grid, crossing bridges, and labels are
painted without adding decorative colors.

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
into the UUID run directory. `CocotbRunnerAdapter` uses the Design++-managed
`$HOME/.designpp/venv/bin/cocotb-config` entrypoint and prepends that venv only
inside the child process. It supports the cocotb 1.9 and 2.x module/filter
environment names, avoids the recursive `sim` Make target, and converts the
Icarus-native FST stream to VCD when that format is selected. Verilator tracing
uses distinct compile and simulation arguments. No process-wide PATH mutation or
Python application logic is added to the repository. The managed installer pins
cocotb to the 1.9 line while the supported APT Verilator remains 5.020. The
bounded cocotb xUnit parser keeps `results.xml` authoritative
when normalization fails. `CocotbRunFinalizer` performs bounded artifact
validation off the GUI thread, always preserves a nonempty xUnit source, writes a
schema-v2 aggregate-and-case summary, and records process success separately from
test-result success. `VerilogWindow` renders current and restored summaries in a
dedicated Tests pane without parsing run artifacts on selection.

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

`TimingRunService` owns the OpenSTA state machine. It selects the newest
successful Yosys run whose schema-v2 synthesis summary has the same RTL/config
fingerprint and ordered Liberty path/content hashes. Older summaries and
generic, Liberty-free netlists remain viewable but are not silently reused for
timing. The selected netlist, managed SDC, and managed Liberty files are copied
to a UUID-scoped ASCII staging directory before the adapter enters the installed
OpenLane 2 Nix environment. The service preserves raw output and available
partial artifacts on process, parsing, or copy failure.

`TimingWindow` is an independent non-Monaco top-level view. It owns only Win32
presentation state and marshals immutable, generation-checked service events to
its UI thread. The window never hashes inputs, copies staging files, invokes
Nix/WSL, or parses reports. OpenSTA exit success and timing-result success are
separate: missing setup/hold paths, negative slack, or reported violations
produce a failed timing run even when the process exits with code zero.

Timing summary schema v2 records nanosecond WNS/TNS values and distinct setup,
hold, recovery, removal, and unknown violation counts. Setup/recovery contribute
to the maximum-delay result, while hold/removal contribute to the minimum-delay
result. `TimingWindow` renders the normalized summary separately from a columnar
violation table. Nix-shell diagnostics remain available in the raw run log but
are not promoted to OpenSTA Problems unless the adapter classifies them as tool
diagnostics.

`TimingRunHistory` reconstructs past OpenSTA presentation snapshots outside the
GUI from RunStore manifests, summaries, scripts, reports, and diagnostics.
`TimingWindow` therefore restores failed and successful runs with the same
corner/WNS/TNS and violation data after restart without performing file parsing in
notification handlers.

`ConstraintsWindow` is a separate top-level managed-file editor for Constraints
views. It hosts Monaco for editable SDC files and does not reuse the
RTL/Testbench `VerilogWindow` or expose lint, simulation, and HDL project
controls. `LibertyWindow` separately previews and manages Library-scoped
technology files. File loading and mutations run through bounded schedulers,
with completion marshalled to the owning window generation.

`ManagedFlowAdapter` is the tool-neutral boundary for complete physical-design
flows. `OpenLane2Adapter` implements the first backend by generating a managed
OpenLane 2 Classic JSON configuration, classifying exact OpenLane step IDs,
normalizing METRICS2.1 values, and discovering immutable state and final views.
Unknown steps and metrics remain preserved in the run summary rather than being
discarded. The adapter does not own processes, RunStore, or Win32 controls.

`ManagedFlowRunService` owns probe, validation, resource acquisition, staging,
backend execution, output collection, artifact validation, and exactly-once
completion. Each Design++ attempt has a new UUID Run, while compatible resumes
reuse a WSL-native lineage workspace under
`$HOME/.designpp/runs/openlane/<project-id>/<lineage-id>`. Resume requires an
unchanged configuration/input fingerprint and a preserved checkpoint. External
CPU use is covered by the cross-process resource coordinator, and all file,
hash, WSL, parsing, and collection work stays off the GUI thread.

Process handles are retired on a bounded cleanup worker after a probe or flow
step callback returns. A completion callback must never destroy the handle whose
`ProcessSession` is currently invoking it, because that would make the process
worker join itself. Window shutdown cancels and joins the active handle before
releasing the service. Backend workspace arguments are canonicalized to absolute
`$HOME` paths before changing into the managed OpenLane installation directory.

OpenLane writes immutable `state_out.json` files inside exact step directories,
not at the run root. Resume therefore discovers the newest state recursively,
verifies its SHA-256 against the Design++ checkpoint manifest, and invokes
`--last-run --from <exact-step-id>` without the mutually exclusive `--run-tag`.

`PhysicalImplementationService` is the tool-neutral EnsureLayout boundary above
`ManagedFlowRunService`. It reuses a compatible non-partial GDS, starts a new
managed flow for stale or missing output, and automatically resumes only an
input-compatible checkpoint. Exact backend step IDs, logs, metrics, and partial
artifacts remain RunStore data and are not promoted into the normal Layout UX.
Physical verification reports from `Magic.DRC`, `Netgen.LVS`, `KLayout.DRC`,
and `KLayout.XOR` are copied into `reports/openlane/` with their backend-relative
step directories and file names intact. They are indexed as `report` artifacts
separately from the normalized JSON summary, including partial reports produced
by failed runs.

`LayoutWindow` is the only user-facing physical implementation window. Opening
it performs only bounded state restoration. Persisted Running or Queued runs
from a previous process lifetime are first recovered as Interrupted, and a
compatible checkpoint from an Interrupted run remains eligible for automatic
resume. Generation starts explicitly from
Generate/Update Layout. The window displays final physical metrics and artifact
state without exposing an OpenLane-branded flow panel. `LayoutViewerService`
ASCII-stages the selected compatible GDS, probes KLayout, and launches it with a
structured WSL command. Viewer or WSLg failure reports an actionable diagnostic
without changing the GDS artifact result.

While a physical implementation is active, Layout presents the live backend,
service state, and current step instead of retaining the summary of a selected
historical Run. The completed Runs table is temporarily read-only and the new
Run is selected after its terminal record is persisted.

`ManagedFlowAdapter` also exposes backend stage capabilities. `OrfsAdapter`
implements the ORFS `synth`, `floorplan`, `place`, `cts`, `route`, and `finish`
targets, checkpoint names, GUI target names, deterministic external
`config.mk` generation, ordered metric parsing, and read-only platform
discovery. `OrfsManagedFlowRunService` is selected by the managed-flow facade
for an `orfs` project and shares staging, resource, cancellation, RunStore,
and exactly-once event ownership with OpenLane. ORFS lineages are retained in
`$HOME/.designpp/runs/orfs/<project-id>/<lineage-id>`. A named cross-process
lease gives one writer exclusive ownership of a lineage while a target is
running. Capability probing requires both the checkout contract and compatible
OpenROAD and Yosys executables. A checkout-local installed toolchain is used
directly when complete; otherwise the adapter enters that same ORFS checkout's
`flake.nix` development environment. The OpenLane Nix shell is never reused for
ORFS because its pinned Yosys/OpenROAD versions need not match the ORFS commit.
The managed installer pins the `26Q2` candidate to commit
`036d106273e66855cd5214d49518fd0f0df7de61` (the peeled commit, not the
annotated tag object's ID), checks its OpenROAD
`0e2d771c5ec38f232493c2afea738ea0200cb972` and Yosys
`d3e297fcd479247322f83d14f42b3556db7acdfb` submodules, and initializes
only those tools and their recursive dependencies. This OpenROAD revision does
not include the later slang-elab dependency that triggered an unprovided fmt
FetchContent download. The pinned environment has completed the public GCD full
flow on nangate45 and sky130hd, including final artifact validation and headless
ODB reload; sky130hd keeps its post-CTS EQY check enabled.
The 26Q2 Yosys flake incorrectly selects nixpkgs ABC rather than its submodule.
An explicit candidate-only recipe patch selects `./abc`, whose commit is checked
as `8e401543d3ecf65e3a3631c7a271793a4d356cb0`, and replaces `nproc` with
`NIX_BUILD_CORES`. The `read_lib -m` capability check remains mandatory; accepting
an incompatible ABC instead would hide a real multi-Liberty synthesis failure.
ABC's GoogleTest 1.14.0 is supplied through a fixed-hash Nix fetcher with
FetchContent disconnected, rather than downloaded from inside the build sandbox.
The same managed bundle pins EQY to
`eff96db01293848b993651caa52d747f191be02e` (2026-03-31) and builds its
plugins against the bundle's Yosys. EQY is mandatory because the sky130hd
configuration enables post-CTS equivalence checking.
Preparation uses cached binaries only (`--max-jobs 0`, no remote builders or
fallback). Missing cache entries require a separately approved source-build
workflow, which is not yet exposed by this installer. Failed candidates and
previous active checkouts are retained for diagnosis and rollback. A clone or
validation failure preserves the active installation. ORFS probe and execution
use offline/cache-only Nix flags; neither may download or compile missing tools.
After preparation succeeds, the candidate receives a `.designpp-environment`
completion marker containing its provider, display version, and exact root
commit. Tool Check validates this marker and the live checkout commits without
entering a Nix shell. A source checkout without a valid marker remains
`preparation required`. The Nix inventory parser accepts both upstream
`nix (Nix)` and Determinate Nix version output.
The preflight validation is read-only and does not invoke `make -n`: recursive
ORFS Make recipes can still execute under dry-run mode and leave a partial
lineage. Only the execution phase creates or mutates the backend workspace.
The top-level ORFS Make graph runs with one Make job because several upstream
recipes produce companion SDC and directory outputs as side effects and are not
safe when sibling targets are scheduled concurrently. The acquired Design++ CPU
token count is passed as `NUM_CORES`, so Yosys and OpenROAD retain bounded
internal parallelism without racing the Make artifact graph.
Generated ORFS `config.mk` assignments are exported so Tcl and shell subprocesses
receive managed values such as `VERILOG_FILES`, `SDC_FILE`, and clock settings in
their environment.
ORFS explicit die/core rectangles and utilization-based floorplan initialization
are mutually exclusive. With both rectangles present, the adapter omits
`CORE_UTILIZATION`. Setup may leave utilization blank in this mode; the draft
retains its previous valid automatic-mode value for a later mode switch.
The upstream ASAP7 platform publishes only corner/model-specific sequential
Liberty variables; the ORFS adapter therefore emits a recursive `DFF_LIB_FILE`
compatibility expression in the generated config so Yosys receives one valid
sequential Liberty without modifying the ORFS checkout.
The ORFS flake's Yosys input can lag behind its synthesis scripts. Probe and
execution both override that input with the checkout's `tools/yosys` flake,
including its pinned ABC submodule, using `--no-write-lock-file`. Capability
checks require Yosys `stat -hierarchy` and ABC `read_lib -m` as well as
`read_liberty -unit_delay`. This prevents an old Yosys/ABC environment from
being reported ready for scripts or compressed multi-Liberty inputs it cannot
handle. Library files and synthesis scripts are not rewritten, and ABC strategy
selection remains the platform default unless explicitly configured by the user.

`OrfsDiscoveryService` marks incomplete platform directories unavailable.
`OpenRoadViewerService` stages a verified ODB and opens it in an independent
OpenROAD/WSLg session; viewer failure does not change the physical Run result.
The Layout Stages dialog is the only incremental-flow entry point and Library
Manager remains a pure library/view browser. Stage reports and artifacts open
from that dialog, and Open in OpenROAD is enabled only when the selected Run
contains a non-empty checkpoint for the selected stage.

ORFS result collection follows the upstream finish contract: final GDS, DEF,
ODB, and gate netlist are required, while a design LEF is optional and is
preserved only when the selected platform emits one. ORFS 26Q2 per-stage JSON
reports use stage-prefixed metric keys (for example,
`finish__design__core__area`); normalization accepts those prefixes and the
upstream `timing__*__ws` spelling while retaining every original key/value
entry in the Run summary. The latest completed stage report is accepted when
upstream does not emit a monolithic `metrics.json`.
Design++ generated ORFS SDC files explicitly use nanoseconds. A managed SDC is
staged with nanoseconds as its default command unit, while a later explicit
`set_cmd_units -time` in that file remains authoritative. Preparation parses
literal `create_clock -period` commands in their effective units, records the
shortest valid clock period in nanoseconds, and uses that value for both ORFS
`CLOCK_PERIOD` and the picosecond `ABC_CLOCK_PERIOD_IN_PS` compatibility input.
Upstream `write_sdc` does not serialize that command unit. Each lineage therefore
contains an executable OpenROAD wrapper and a private initialization Tcl file.
The wrapper finds the existing Tcl command-file argument without disturbing
trailing options such as `-metrics`, then replaces that argument with a temporary
Tcl file that sources the unit initialization before the original ORFS script.
This does not depend on OpenROAD's current-directory `.openroad` lookup and
reapplies the prepared SDC's final command unit before every
synthesis, floorplan, placement, CTS, routing, final, resume, or GUI process
reads an intermediate SDC. The installed ORFS checkout and the user's SDC are
not modified. Each intercepted read emits a run-local marker. A process that
otherwise succeeds without that marker fails the managed-flow contract with
`ORFS-SDC-UNIT` instead of publishing potentially mis-scaled timing data.
The Setup default therefore cannot override a managed SDC clock. A design with
no clock receives no ABC clock constraint. Timing metrics retain their raw keys
and values; normalized Layout timing values are converted to nanoseconds only
when the staged SDC time unit is known.
Successful Nix invocations can report the complete temporary input-override
diff for every `--no-write-lock-file` call. ORFS keeps those bytes in the raw
Run log but removes that known multiline notice from live UI output; all other
Nix and child-process warnings remain visible.

Project schema v10 keeps power-distribution and I/O-placement intent in the tool-neutral physical
implementation configuration. Boolean grid topology controls and optional
vertical/horizontal width, spacing, pitch, and offset values are edited in a
separate Layout PDN dialog. Empty numeric values inherit PDK defaults. The
OpenLane adapter owns translation to `FP_PDN_*`, and every PDN value is included
in the configuration fingerprint so an incompatible checkpoint is never reused.
Absolute floorplans may persist both die and core rectangles; a core rectangle
is accepted only when paired with and fully contained by its die rectangle.
The optional tap-cell column distance is stored in micrometers and maps to
OpenLane `FP_TAPCELL_DIST`; an empty value deliberately inherits the selected
PDK/SCL default.

The Layout Setup window is a DPI-scaled, resizable tabbed editor. Form edits stay
in its draft until the parent Save succeeds; JSON Apply only updates that draft
and the parent Save owns persistence. The I/O page keeps common settings visible and uses a
nested N/S/E/W tab to edit one side without crowding the other directions.
Structured I/O settings map to `FP_PPL_MODE`, `FP_IO_*`,
and `ERRORS_ON_UNMATCHED_IO`; custom N/S/E/W entries deterministically generate
a run-local ASCII-safe `pin_order.cfg`. Both structured values and generated CFG
content participate in the physical configuration fingerprint. The OpenLane
adapter's typed JSON codec is the only JSON-to-configuration boundary: known
keys update the draft, safe unknown flat values remain advanced overrides, and
managed execution, RTL, SDC, path, nested, duplicate, or oversized input is
rejected before persistence. OpenLane-style numeric `DIE_AREA` and `CORE_AREA`
arrays are accepted and normalized to the project model; `FP_PIN_ORDER_CFG`
remains run-owned and is generated only from the directional I/O editor. A
resolved-config snapshot may contain that key; the editor ignores its path
without reading or persisting it and still applies the remaining typed values.
The JSON surface reuses `MonacoEditorHost` with an in-memory `json` model. Text
is requested only for Validate, Format, Apply, Save Setup, or Ctrl+S, so editing
does not copy the full document through Win32 on every keystroke. Apply updates
the parent dialog draft. The parent Save publishes the validated typed
configuration to the Layout window's asynchronous atomic project-save path while
the JSON editor and Setup dialog remain open. Save never sends `WM_CLOSE` or
destroys a WebView2 controller. Explicit Cancel, title-bar close, and editor close
requests are posted back to the Win32 message queue and deduplicated; the
controller is never destroyed reentrantly while WebView2 is still delivering its
callback. Explicit closure uses two queued phases: the WebView2 controller is shut
down while its parent HWND is still valid, and the HWND is destroyed only on the
following message.

Physical setup is cell-scoped. Each Layout window opens the selected cell's
`project.dpproj`, and asynchronous setup saves carry an immutable project snapshot;
the worker writes only that snapshot and the owning UI thread publishes it after a
successful revision-checked save. Run discovery and artifact compatibility also
require the current project ID, so copied or stale run directories cannot make one
cell's setup appear in another cell.
When the Library snapshot is refreshed, each Layout session invalidates its
pending worker events, cancels the old backend/viewer generation, and clears
the loaded document, runs, and controls before resolving the same Cell again.
This prevents a queued result from the previously selected Cell from being
published into the refreshed window.
Library validation rejects case-insensitive duplicate Cell/View IDs because those
IDs are Windows directory names; this prevents two manifest entries from aliasing
the same project or view storage.

All WSLg viewer launches pass through `WslGuiExecutionService`. Before starting
KLayout or GTKWave, the service silently checks whether the selected
distribution user can write to `/mnt/shared_memory`. A root-owned 0755 directory
is repaired to sticky world-writable mode 1777 first. If that permission repair
cannot produce a user-writable transport, the service falls back to mounting a
distribution-local `tmpfs` with mode 1777, verifies the repair, and only then
launches the viewer. It never issues
`wsl --shutdown`, so unrelated terminals and EDA runs are not interrupted.
Health checks, repair, viewer execution, cancellation, and exactly-once
completion remain behind the structured `ExecutionProvider` boundary.
