# Copyright 2026 The Design++ Authors

param(
  [Parameter(Mandatory = $true)][string]$Gds,
  [Parameter(Mandatory = $true)][string]$Cdl,
  [Parameter(Mandatory = $true)][string]$Top,
  [Parameter(Mandatory = $true)][string]$Recipe,
  [Parameter(Mandatory = $true)][string]$Driver,
  [string]$KLayout = "/usr/bin/klayout"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$fixture = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot "expectations.json") |
  ConvertFrom-Json
$generatorWindows = Join-Path $PSScriptRoot "generate_variant.rb"
$generator = (& wsl.exe --exec wslpath -a $generatorWindows).Trim()
if ($LASTEXITCODE -ne 0 -or !$generator) {
  throw "Could not map the fixture generator into WSL"
}

$runId = [guid]::NewGuid().ToString()
$root = "/tmp/designpp-sky130hd-lvs-$runId"
& wsl.exe --exec mkdir -p $root
if ($LASTEXITCODE -ne 0) { throw "Could not create fixture directory $root" }

$failures = @()
foreach ($case in $fixture.variants) {
  $variant = [string]$case.id
  $expected = [string]$case.expected
  $variantGds = "$root/$variant.gds"
  $report = "$root/$variant.lvsdb"
  $caseRecipe = $Recipe

  $generationOutput = & wsl.exe --exec $KLayout -b `
    -rd "input_gds=$Gds" -rd "output_gds=$variantGds" `
    -rd "variant=$variant" -r $generator 2>&1
  if ($LASTEXITCODE -ne 0) {
    $failures += "${variant}: fixture generation failed"
    $generationOutput | ForEach-Object { Write-Output $_ }
    continue
  }

  if ($variant -eq "altered_rule_hash") {
    $caseRecipe = "$root/altered-rule.lylvs"
    & wsl.exe --exec cp $Recipe $caseRecipe
    $ruleSize = [int64](& wsl.exe --exec stat -c %s $caseRecipe)
    & wsl.exe --exec truncate -s ($ruleSize + 1) $caseRecipe
    if ($LASTEXITCODE -ne 0) {
      $failures += "${variant}: rule mutation failed"
      continue
    }
  }

  $engineOutput = & wsl.exe --exec $KLayout -b `
    -rd "in_gds=$variantGds" -rd "cdl_file=$Cdl" `
    -rd "report_file=$report" -rd "target_netlist=$report.spice" `
    -rd "recipe_path=$caseRecipe" -rd "recipe_contract=sky130hd-bulk-v1" `
    -rd "top_cell=$Top" -r $Driver 2>&1
  $engineExit = $LASTEXITCODE

  $summary = ""
  & wsl.exe --exec test -f "$report.summary"
  if ($LASTEXITCODE -eq 0) {
    $summary = (& wsl.exe --exec cat "$report.summary").Trim()
  }
  $parts = $summary -split " "
  $passed = $parts.Count -eq 4 -and $parts[0] -eq "DESIGNPP_LVS_XREF_V1" -and
    [int]$parts[2] -eq 0 -and [int]$parts[3] -eq 0
  $matched = switch ($expected) {
    "pass" { $engineExit -eq 0 -and $passed }
    "mismatch" { $engineExit -eq 0 -and $summary -ne "" -and !$passed }
    "preflight_rejection" { $engineExit -ne 0 -and $summary -eq "" }
    default { $false }
  }
  if (!$matched) {
    $failures +=
      "${variant}: expected $expected, exit=$engineExit, summary=$summary"
    $engineOutput | ForEach-Object { Write-Output $_ }
  }
  Write-Output "$variant expected=$expected exit=$engineExit summary=$summary"
}

Write-Output "Fixture outputs: $root"
if ($failures.Count -gt 0) {
  $failures | ForEach-Object { Write-Output "FAILED: $_" }
  exit 1
}
