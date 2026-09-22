# sky130hd LVS engine regression fixture

This opt-in fixture checks the hash-bound `sky130hd-bulk-v1` contract against a
known passing GDS/CDL pair. It creates every variant under a new UUID directory
in WSL `/tmp`; it never edits the source Run, installed PDK, rule, or model.

The matrix requires the clean control to pass, signal short/open, supply short,
and tap-contact removal to produce comparison evidence that is not a pass, and
deep-well or missing-boundary inputs to be rejected before comparison. The
tracked source-rule hash is also mutated in an isolated copy and must be rejected.
The tracked fixture contains only transformations and expectations. PDK data and
design artifacts remain external.

Run from the repository root with WSL paths for the immutable inputs. `Driver`
is the `driver.rb` preserved by a production-service sky130hd LVS Run, ensuring
the fixture exercises the same driver that produced the recorded Run evidence.

```powershell
& tests/fixtures/sky130hd_lvs/run.ps1 `
  -Gds /path/to/source-run/artifacts/final.gds `
  -Cdl /path/to/verification-run/reports/cdl/combined.cdl `
  -Top timer_1s `
  -Recipe /home/user/.designpp/toolchains/orfs/flow/platforms/sky130hd/lvs/sky130hd.lylvs `
  -Driver /path/to/verification-run/reports/driver.rb
```

The runner exits nonzero if any observed result differs from
`expectations.json`. Retain a production Run ID and its source Run ID with test
records; `/tmp` outputs are disposable diagnostics rather than authoritative
Run artifacts.
