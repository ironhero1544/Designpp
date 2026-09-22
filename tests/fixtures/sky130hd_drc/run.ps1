# Copyright 2026 The Design++ Authors

param(
  [Parameter(Mandatory = $true)][string]$Gds,
  [Parameter(Mandatory = $true)][string]$Recipe,
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
$root = "/tmp/designpp-sky130hd-drc-$runId"
& wsl.exe --exec mkdir -p $root
if ($LASTEXITCODE -ne 0) { throw "Could not create fixture directory $root" }

$failures = @()
foreach ($case in $fixture.variants) {
  $variant = [string]$case.id
  $variantGds = "$root/$variant.gds"
  $report = "$root/$variant.lyrdb"
  $generationOutput = & wsl.exe --exec $KLayout -b `
    -rd "input_gds=$Gds" -rd "output_gds=$variantGds" `
    -rd "variant=$variant" -r $generator 2>&1
  if ($LASTEXITCODE -ne 0) {
    $failures += "${variant}: fixture generation failed"
    $generationOutput | ForEach-Object { Write-Output $_ }
    continue
  }

  $engineOutput = & wsl.exe --exec $KLayout -b `
    -rd "in_gds=$variantGds" -rd "report_file=$report" -r $Recipe 2>&1
  $engineExit = $LASTEXITCODE
  $xml = ""
  & wsl.exe --exec test -s $report
  if ($LASTEXITCODE -eq 0) {
    $xml = (& wsl.exe --exec cat $report) -join "`n"
  }
  $violations = ([regex]::Matches($xml, "<item>")).Count
  $maximum = if ($case.PSObject.Properties.Name -contains
      "maximum_violations") {
    [int]$case.maximum_violations
  } else {
    [int]::MaxValue
  }
  $matched = $engineExit -eq 0 -and $xml -ne "" -and
    $violations -ge [int]$case.minimum_violations -and $violations -le $maximum
  if (!$matched) {
    $failures += "${variant}: exit=$engineExit, violations=$violations"
    $engineOutput | ForEach-Object { Write-Output $_ }
  }
  Write-Output "$variant exit=$engineExit violations=$violations"
}

Write-Output "Fixture outputs: $root"
if ($failures.Count -gt 0) {
  $failures | ForEach-Object { Write-Output "FAILED: $_" }
  exit 1
}
