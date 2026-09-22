param(
  [ValidatePattern("^\d+\.\d+\.\d+$")]
  [string]$Version = "1.0.0",
  [string]$MakeNsis = "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
)
$ErrorActionPreference = "Stop"
$repository = Split-Path -Parent $PSScriptRoot
$releaseRoot = Join-Path $repository "artifacts\release"
$payload = Join-Path $releaseRoot "Design++-$Version-x64"
$manifestPath = Join-Path $payload "release-manifest.json"
if (-not (Test-Path -LiteralPath $MakeNsis)) { throw "Install NSIS 3 or supply -MakeNsis." }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.version -ne $Version -or $manifest.configuration -ne "Release") {
  throw "Create a matching Release package with package_release.ps1 first."
}
# Enumerate only shipped files. Never recursively delete the install directory.
$removal = Join-Path $releaseRoot "uninstall-$Version.nsh"
$lines = [Collections.Generic.List[string]]::new()
foreach ($file in Get-ChildItem -LiteralPath $payload -Recurse -File) {
  $relative = $file.FullName.Substring($payload.Length + 1)
  if ($relative -match '["$\r\n]') { throw "Unsupported NSIS payload path: $relative" }
  $lines.Add('Delete "$INSTDIR\' + $relative + '"')
}
foreach ($directory in Get-ChildItem -LiteralPath $payload -Recurse -Directory | Sort-Object { $_.FullName.Length } -Descending) {
  $relative = $directory.FullName.Substring($payload.Length + 1)
  if ($relative -match '["$\r\n]') { throw "Unsupported NSIS directory: $relative" }
  $lines.Add('RMDir "$INSTDIR\' + $relative + '"')
}
[IO.File]::WriteAllLines($removal, $lines, [Text.UTF8Encoding]::new($true))
$installer = Join-Path $releaseRoot "Design++-$Version-x64-Setup.exe"
& $MakeNsis /V2 "/DVERSION=$Version" "/DPAYLOAD=$payload" "/DOUTPUT=$installer" "/DREMOVAL=$removal" (Join-Path $PSScriptRoot "installer\designpp.nsi")
if ($LASTEXITCODE -ne 0) { throw "NSIS failed with exit $LASTEXITCODE" }
$hash = Get-FileHash -LiteralPath $installer -Algorithm SHA256
($hash.Hash.ToLowerInvariant() + "  " + (Split-Path -Leaf $installer)) |
  Set-Content -LiteralPath ($installer + ".sha256") -Encoding ascii
Get-Item -LiteralPath $installer, ($installer + ".sha256")
