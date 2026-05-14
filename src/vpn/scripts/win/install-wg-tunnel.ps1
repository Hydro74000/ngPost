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
& $wg /installtunnelservice $ConfPath
if ($LASTEXITCODE -ne 0) {
    Write-Error "wireguard.exe /installtunnelservice failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

$baseName = [System.IO.Path]::GetFileNameWithoutExtension($ConfPath)
$svc = "WireGuardTunnel`$$baseName"

# Wait briefly for the service to appear in SCM (sometimes lags by a moment).
for ($i = 0; $i -lt 20; $i++) {
    sc.exe query $svc | Out-Null
    if ($LASTEXITCODE -eq 0) { break }
    Start-Sleep -Milliseconds 250
}
if ($LASTEXITCODE -ne 0) {
    Write-Error "Service $svc did not appear after install"
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
# SERVICE_STOP (WP) to the invoking user.
$raw = sc.exe sdshow $svc
$dline = $raw | Where-Object { $_ -match "^D:" } | Select-Object -First 1
$sline = $raw | Where-Object { $_ -match "^S:" } | Select-Object -First 1
if (-not $dline) {
    Write-Error "Service SDDL has no DACL: $raw"
    exit 6
}

$newAce = "(A;;RPWP;;;$sid)"
$newDacl = $dline + $newAce
$newSddl = $newDacl
if ($sline) {
    $newSddl = "$newDacl$sline"
}

sc.exe sdset $svc $newSddl
if ($LASTEXITCODE -ne 0) {
    Write-Error "sc.exe sdset failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

Write-Output "INSTALLED $svc for $InvokerUser"
exit 0
