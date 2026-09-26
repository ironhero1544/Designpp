$ErrorActionPreference = "Stop"
$bootstrapper = Join-Path $PSScriptRoot "MicrosoftEdgeWebView2Setup.exe"
$step = "download"

try {
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  Invoke-WebRequest -Uri "https://go.microsoft.com/fwlink/p/?LinkId=2124703" `
    -OutFile $bootstrapper -UseBasicParsing

  $step = "signature verification"
  $signature = Get-AuthenticodeSignature -LiteralPath $bootstrapper
  if ($signature.Status -ne "Valid" -or
      $signature.SignerCertificate.Subject -notmatch "(^|, )O=Microsoft Corporation(,|$)") {
    throw "The downloaded bootstrapper does not have a valid Microsoft signature."
  }

  $step = "installation"
  $process = Start-Process -FilePath $bootstrapper -ArgumentList "/silent", "/install" `
    -Wait -PassThru -WindowStyle Hidden
  if ($process.ExitCode -ne 0) {
    throw "Microsoft bootstrapper exited with code $($process.ExitCode)."
  }
  Write-Output "Microsoft WebView2 Runtime bootstrapper completed."
} catch {
  Write-Output "WebView2 $step failed: $($_.Exception.Message)"
  exit 1
} finally {
  Remove-Item -LiteralPath $bootstrapper -Force -ErrorAction SilentlyContinue
}
