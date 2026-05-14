# uninstall-wg-tunnel.ps1 — remove a WireGuard tunnel service previously
# registered via install-wg-tunnel.ps1. Called by ngPost (via UAC) when the
# user deletes a WireGuard profile.

param(
    [Parameter(Mandatory=$true)]
    [string]$ServiceName  # e.g., WireGuardTunnel$MyProfile
)

$ErrorActionPreference = "Continue"

# Tunnel name = service name with the WireGuardTunnel$ prefix stripped.
$tunnelName = $ServiceName -replace '^WireGuardTunnel\$', ''

$candidates = @(
    "C:\Program Files\WireGuard\wireguard.exe",
    "C:\Program Files (x86)\WireGuard\wireguard.exe"
)
$wg = $null
foreach ($p in $candidates) {
    if (Test-Path $p) { $wg = $p; break }
}

if ($wg) {
    & $wg /uninstalltunnelservice $tunnelName
} else {
    # Fallback: delete the service by name via sc.exe. The tunnel will be torn
    # down at next reboot if it was running; not ideal but better than leaking.
    sc.exe stop   $ServiceName 2>$null | Out-Null
    sc.exe delete $ServiceName 2>$null | Out-Null
}

Write-Output "UNINSTALLED $ServiceName"
exit 0
