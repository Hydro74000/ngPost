<#
.SYNOPSIS
  Fetch the pinned PAR2 tools for Windows into one directory.

.DESCRIPTION
  The Windows counterpart of fetch-par2-tools.sh, reading the same pin list
  (.github/scripts/par2-tools.env) so no runner can end up on a different build
  of the same tool.

  Used by both tests and release packaging. ParParDestination allows the
  installer to stage its optional ParPar component separately. VerifyOnly
  checks the staged tools without downloading or changing them.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string] $Destination,
  [string] $ParParDestination,
  [switch] $VerifyOnly
)

$ErrorActionPreference = 'Stop'

$pins = Join-Path $PSScriptRoot 'par2-tools.env'
if (-not (Test-Path $pins)) { throw "Missing pin list: $pins" }
$pin = @{}
Get-Content $pins | Where-Object { $_ -match '^([A-Z0-9_]+)=(.*)$' } | ForEach-Object {
  if ($_ -match '^([A-Z0-9_]+)=(.*)$') { $pin[$Matches[1]] = $Matches[2] }
}

if (-not $ParParDestination) { $ParParDestination = $Destination }
$Destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
$ParParDestination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ParParDestination)
$parparVersion = $pin['PARPAR_VERSION']
$par2Version = $pin['PAR2CMDLINE_VERSION']
$mpVersion = $pin['MULTIPAR_VERSION']

function Get-Verified([string] $Url, [string] $Sha256, [string] $Name) {
  $file = Join-Path $tmp $Name
  Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $file
  $got = (Get-FileHash $file -Algorithm SHA256).Hash.ToLower()
  if ($got -ne $Sha256) { throw "$Name SHA-256 mismatch: got $got, expected $Sha256" }
  return $file
}

function Copy-FromArchive([string] $Root, [string] $Filter, [string] $Target) {
  $found = Get-ChildItem $Root -Recurse -File -Filter $Filter | Select-Object -First 1
  if (-not $found) { throw "$Filter not found in the downloaded archive" }
  Copy-Item $found.FullName $Target -Force
}

function Test-Tool([string] $Executable, [string] $Arguments, [string] $Pattern) {
  # .NET keeps native stderr out of PowerShell 5's error stream. In particular,
  # ParPar writes its version there even on success. Check the exit code too:
  # a DLL loader error must not pass merely because it produced some output.
  if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Missing PAR2 tool: $Executable" }
  $process = New-Object System.Diagnostics.Process
  $process.StartInfo.FileName = $Executable
  $process.StartInfo.Arguments = $Arguments
  $process.StartInfo.UseShellExecute = $false
  $process.StartInfo.CreateNoWindow = $true
  $process.StartInfo.RedirectStandardOutput = $true
  $process.StartInfo.RedirectStandardError = $true
  try {
    if (-not $process.Start()) { throw "Cannot start $Executable" }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) {
      $process.Kill()
      throw "PAR2 tool timed out: $Executable"
    }
    $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0 -or $output -notmatch $Pattern) {
      throw "PAR2 tool check failed: $Executable (exit $($process.ExitCode)): $output"
    }
    Write-Host "$(Split-Path $Executable -Leaf): $(($output -split "`n")[0].Trim())"
  } finally {
    $process.Dispose()
  }
}

if (-not $VerifyOnly) {
  New-Item -ItemType Directory -Force -Path $Destination, $ParParDestination | Out-Null
  $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("par2-tools-" + [System.Guid]::NewGuid())
  New-Item -ItemType Directory -Force -Path $tmp | Out-Null
  try {
    # ParPar - .7z, so 7-Zip rather than Expand-Archive (preinstalled on the runners).
    $asset = "parpar-v$parparVersion-win64.7z"
    $archive = Get-Verified "https://github.com/animetosho/ParPar/releases/download/v$parparVersion/$asset" $pin['PARPAR_SHA256_WIN64'] $asset
    $sevenZip = "C:\Program Files\7-Zip\7z.exe"
    if (-not (Test-Path $sevenZip)) { throw "7-Zip not found at $sevenZip - runner image changed?" }
    $extract = Join-Path $tmp 'parpar'
    & $sevenZip x $archive "-o$extract" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7-Zip failed to extract $asset (exit $LASTEXITCODE)" }
    Copy-FromArchive $extract 'parpar.exe' (Join-Path $ParParDestination 'parpar.exe')

    # par2cmdline
    $asset = "par2cmdline-$par2Version-win-x64.zip"
    $archive = Get-Verified "https://github.com/Parchive/par2cmdline/releases/download/v$par2Version/$asset" $pin['PAR2CMDLINE_SHA256_WIN_X64'] $asset
    $extract = Join-Path $tmp 'par2cmdline'
    Expand-Archive -Path $archive -DestinationPath $extract -Force
    Copy-FromArchive $extract 'par2.exe' (Join-Path $Destination 'par2.exe')

    # MultiPar (par2j64.exe) - Windows only.
    $asset = $pin['MULTIPAR_ASSET']
    $archive = Get-Verified "https://github.com/Yutaka-Sawada/MultiPar/releases/download/v$mpVersion/$asset" $pin['MULTIPAR_SHA256'] $asset
    $extract = Join-Path $tmp 'multipar'
    Expand-Archive -Path $archive -DestinationPath $extract -Force
    Copy-FromArchive $extract 'par2j64.exe' (Join-Path $Destination 'par2j64.exe')

    # par2.exe is an MSVC/OpenMP build and does not start without this runtime.
    $vcomp = $null
    if ($env:VCToolsRedistDir) {
      $vcomp = Get-ChildItem $env:VCToolsRedistDir -Recurse -File -Filter 'vcomp140.dll' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
    }
    if (-not $vcomp) {
      $vcomp = Get-ChildItem (Join-Path $env:SystemRoot 'System32') -File -Filter 'vcomp140.dll' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    }
    if (-not $vcomp) { throw "vcomp140.dll not found: par2.exe (MSVC/OpenMP build) would not start" }
    Copy-Item $vcomp.FullName (Join-Path $Destination 'vcomp140.dll') -Force

  } finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
  }
}

if (-not (Test-Path (Join-Path $Destination 'vcomp140.dll') -PathType Leaf)) {
  throw 'Missing bundled OpenMP runtime: vcomp140.dll'
}
Test-Tool (Join-Path $ParParDestination 'parpar.exe') '--version' ("\A\s*" + [regex]::Escape($parparVersion) + "\s*\z")
Test-Tool (Join-Path $Destination 'par2.exe') '-V' ("(?m)^par2cmdline version " + [regex]::Escape($par2Version) + "\r?$")
# MultiPar's release version and its par2j client version can differ.
$mpClientVersion = $pin['MULTIPAR_CLIENT_VERSION']
Test-Tool (Join-Path $Destination 'par2j64.exe') '' ("(?s)\AParchive 2\.0 client version " + [regex]::Escape($mpClientVersion) + " by Yutaka Sawada\r?\n\s*Self-Test: Success")

# Hand the pins to the rest of the job, as the Unix script does.
if ($env:GITHUB_ENV) {
  Get-Content $pins | Where-Object { $_ -match '^[A-Z0-9_]+=' } | Add-Content -Path $env:GITHUB_ENV -Encoding utf8
}

Write-Host "Bundled ParPar $parparVersion, par2cmdline $par2Version and MultiPar $mpVersion into $Destination"
# 7-Zip's exit code must not leak into a calling GitHub Actions step.
$global:LASTEXITCODE = 0
