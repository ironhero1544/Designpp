param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Release",
  [ValidatePattern("^\d+\.\d+\.\d+$")]
  [string]$Version = "1.0.0"
)

$ErrorActionPreference = "Stop"
$repository = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repository ("x64\" + $Configuration)
$releaseRoot = Join-Path $repository "artifacts\release"
$stage = Join-Path $releaseRoot ("Design++-" + $Version + "-x64")
$archive = $stage + ".zip"

if (-not (Test-Path -LiteralPath (Join-Path $output "Design++.exe"))) {
  throw "Build x64 $Configuration before packaging."
}

$resolvedRoot = [IO.Path]::GetFullPath($releaseRoot) + [IO.Path]::DirectorySeparatorChar
if (-not [IO.Path]::GetFullPath($stage).StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Package staging path escapes release root."
}
if ($Configuration -eq "Release" -and
    (Get-Item -LiteralPath (Join-Path $output "Design++.exe")).VersionInfo.ProductVersion -ne $Version) {
  throw "Executable version does not match package version. Rebuild the matching resource version."
}
New-Item -ItemType Directory -Force -Path $releaseRoot | Out-Null
if (Test-Path -LiteralPath $stage) {
  Remove-Item -LiteralPath $stage -Recurse -Force
}
if (Test-Path -LiteralPath $archive) {
  Remove-Item -LiteralPath $archive -Force
}
New-Item -ItemType Directory -Path $stage | Out-Null

$files = @(
  "Design++.exe",
  "WebView2Loader.dll",
  "WebView2-LICENSE.txt",
  "WebView2-NOTICE.txt",
  "THIRD_PARTY_NOTICES.md"
)
foreach ($file in $files) {
  $source = Join-Path $output $file
  if (-not (Test-Path -LiteralPath $source)) {
    throw "Required release file is missing: $source"
  }
  Copy-Item -LiteralPath $source -Destination $stage
}
$editorAssets = Join-Path $output "assets\editor"
if (-not (Test-Path -LiteralPath (Join-Path $editorAssets "main.js"))) {
  throw "Bundled Monaco assets are missing: $editorAssets"
}
Copy-Item -LiteralPath (Join-Path $output "assets") -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $repository "docs\INSTALL.md") -Destination $stage

Copy-Item -LiteralPath (Join-Path $repository "README.md") -Destination $stage
Copy-Item -LiteralPath (Join-Path $repository "README_ko.md") -Destination $stage
Copy-Item -LiteralPath (Join-Path $repository "LICENSE") -Destination $stage
Copy-Item -LiteralPath (Join-Path $repository "docs") -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $repository "licenses") -Destination $stage -Recurse

$manifest = [ordered]@{
  schema_version = 1
  product = "Design++"
  version = $Version
  platform = "x64-windows"
  configuration = $Configuration
  supported_toolchains = @(
    [ordered]@{ provider = "openlane2"; bundle = "openlane2-2.3.10"; contract = "openlane2-classic-v1" },
    [ordered]@{ provider = "orfs"; bundle = "orfs-26Q2"; contract = "orfs-26q2-v1" }
  )
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage "release-manifest.json") -Encoding utf8
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
$hash = Get-FileHash -LiteralPath $archive -Algorithm SHA256
($hash.Hash.ToLowerInvariant() + "  " + (Split-Path -Leaf $archive)) |
  Set-Content -LiteralPath ($archive + ".sha256") -Encoding ascii
Get-Item -LiteralPath $archive, ($archive + ".sha256")
