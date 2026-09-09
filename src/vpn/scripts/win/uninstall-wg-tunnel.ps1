# uninstall-wg-tunnel.ps1 — remove a WireGuard tunnel service previously
# registered via install-wg-tunnel.ps1. Called by ngPost (via UAC) when the
# user deletes a WireGuard profile.

param(
    [Parameter(Mandatory=$true)]
    [string]$ServiceName  # e.g., WireGuardTunnel$MyProfile
)

$ErrorActionPreference = "Continue"

# Tunnel name = service name with the "WireGuardTunnel$" prefix stripped.
# Using SubString instead of regex to avoid escaping the literal $ correctly.
$prefix = 'WireGuardTunnel$'
if ($ServiceName.StartsWith($prefix)) {
    $tunnelName = $ServiceName.Substring($prefix.Length)
} else {
    $tunnelName = $ServiceName
}

Write-Host "Uninstalling tunnel: '$tunnelName' (service: '$ServiceName')"

$candidates = @(
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32)) {
        $machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $view)
        $key = $null
        try {
            $key = $machine.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion')
            foreach ($name in @('ProgramFilesDir', 'ProgramFilesDir (x86)')) {
                $root = if ($key) { $key.GetValue($name) } else { $null }
                if ($root -and [IO.Path]::IsPathRooted($root)) {
                    Join-Path $root 'WireGuard\wireguard.exe'
                }
            }
        } finally {
            if ($key) { $key.Dispose() }
            $machine.Dispose()
        }
    }
)
$wg = $null
foreach ($p in $candidates) {
    if (Test-Path -LiteralPath $p -PathType Leaf) { $wg = $p; break }
}

$removed = $false
if ($wg) {
    & $wg /uninstalltunnelservice $tunnelName
    # Poll briefly for the service to actually disappear from SCM.
    for ($i = 0; $i -lt 20; $i++) {
        sc.exe query $ServiceName 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) { $removed = $true; break }
        Start-Sleep -Milliseconds 250
    }
}

if (-not $removed) {
    # Fallback: stop and delete the service ourselves.
    sc.exe stop   $ServiceName 2>$null | Out-Null
    Start-Sleep -Seconds 1
    sc.exe delete $ServiceName 2>$null | Out-Null
    for ($i = 0; $i -lt 20; $i++) {
        sc.exe query $ServiceName 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) { $removed = $true; break }
        Start-Sleep -Milliseconds 250
    }
}

if (-not $removed) {
    Write-Error "Service $ServiceName still present after uninstall attempts"
    exit 1
}

Write-Output "UNINSTALLED $ServiceName"
exit 0
