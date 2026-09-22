# sky130hd DRC engine regression fixture

This opt-in fixture runs the installed sky130hd KLayout DRC rule against a
known clean GDS and an isolated copy containing a deliberately narrow met1
shape. Each invocation creates a new UUID directory under WSL `/tmp`; the source
Run, installed PDK, and rule are read-only inputs.

```powershell
& tests/fixtures/sky130hd_drc/run.ps1 `
  -Gds /path/to/source-run/artifacts/final.gds `
  -Recipe /home/user/.designpp/toolchains/orfs/flow/platforms/sky130hd/drc/sky130hd.lydrc
```

The clean control must contain zero report items. The narrow-met1 variant must
contain at least one marker. Both reports must exist and be nonempty.
