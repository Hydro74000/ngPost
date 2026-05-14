# install-wg-tunnel.ps1 — register a WireGuard tunnel service for ngPost.
#
# Called by ngPost (via UAC elevation) when the user creates or edits a
# WireGuard profile on Windows. The work happens in two steps:
#   1. wireguard.exe /installtunnelservice <conf>  — creates a Windows
#      service named "WireGuardTunnel$<basename-of-conf>" running as SYSTEM.
#   2. sc sdset <service> ...                       — extends the service ACL
#      to grant SERVICE_START + SERVICE_STOP to the invoking user, so that
#      subsequent runtime connect/disconnect from ngPost (unprivileged) does
#      NOT require UAC.
#
# Exit codes : 0 = success, non-zero = failure (message written to stderr).

param(
    [Parameter(Mandatory=$true)]
    [string]$ConfPath,

    [Parameter(Mandatory=$true)]
    [string]$InvokerUser
)

$ErrorActionPreference = "Stop"

function Find-Wireguard {
    $candidates = @(
        "C:\Program Files\WireGuard\wireguard.exe",
        "C:\Program Files (x86)\WireGuard\wireguard.exe"
    )
    foreach ($p in $candidates) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

$wg = Find-Wireguard
if (-not $wg) {
    Write-Error "wireguard.exe not found. Install WireGuard for Windows first."
    exit 2
}

if (-not (Test-Path $ConfPath)) {
    Write-Error "Config file not found: $ConfPath"
    exit 3
}

# Step 1: register the tunnel as a Windows service.
# wireguard.exe is a Windows GUI-subsystem binary; it forks the service into
# the background and exits without setting a reliable $LASTEXITCODE on the
# caller. So we don't trust the exit code — instead we look for the service
# to appear in SCM as evidence of success.
& $wg /installtunnelservice $ConfPath

$baseName = [System.IO.Path]::GetFileNameWithoutExtension($ConfPath)
$svc = "WireGuardTunnel`$$baseName"

$found = $false
for ($i = 0; $i -lt 40; $i++) {
    sc.exe query $svc 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) { $found = $true; break }
    Start-Sleep -Milliseconds 250
}
if (-not $found) {
    Write-Error "Service $svc did not appear after wireguard.exe /installtunnelservice. The .conf may be invalid."
    exit 4
}

# Step 2: extend the service ACL.
# Resolve the invoker's SID from the username.
try {
    $sid = (New-Object System.Security.Principal.NTAccount($InvokerUser)).Translate(
        [System.Security.Principal.SecurityIdentifier]
    ).Value
} catch {
    Write-Error "Could not resolve SID for user '$InvokerUser': $_"
    exit 5
}

# Read the current SDDL and append an ACE granting SERVICE_START (RP) and
# SERVICE_STOP (WP) to the invoking user. sc.exe sdshow returns the whole
# SDDL on a single line (with leading blank line); collapse to one string
# and split D: from S: at the ")S:" boundary (a literal `S:` inside an ACE
# flag mnemonic like CCLCSW does NOT end the DACL, but the closing paren
# of the last DACL ACE does).
$raw = ((sc.exe sdshow $svc) -join '') -replace '\s', ''
if (-not $raw.StartsWith('D:')) {
    Write-Error "Unexpected SDDL (missing DACL): $raw"
    exit 6
}
$sBoundary = $raw.IndexOf(')S:')
if ($sBoundary -ge 0) {
    $dPart = $raw.Substring(0, $sBoundary + 1)
    $sPart = $raw.Substring($sBoundary + 1)
} else {
    $dPart = $raw
    $sPart = ''
}

# Idempotency: don't append the ACE twice if the script is re-run.
$newAce = "(A;;RPWP;;;$sid)"
if ($dPart -notlike "*$newAce*") {
    $dPart = $dPart + $newAce
}
$newSddl = $dPart + $sPart

sc.exe sdset $svc $newSddl
if ($LASTEXITCODE -ne 0) {
    Write-Error "sc.exe sdset failed (exit $LASTEXITCODE) for SDDL: $newSddl"
    exit $LASTEXITCODE
}

Write-Output "INSTALLED $svc for $InvokerUser"
exit 0
