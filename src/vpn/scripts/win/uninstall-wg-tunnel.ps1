# uninstall-wg-tunnel.ps1 — remove a WireGuard tunnel service previously
# registered via install-wg-tunnel.ps1. Called by ngPost (via UAC) when the
# user deletes a WireGuard profile.

param(
    [Parameter(Mandatory=$true)]
    [string]$ServiceName  # e.g., WireGuardTunnel$MyProfile
)

$ErrorActionPreference = "Stop"

# Tunnel name = service name with the "WireGuardTunnel$" prefix stripped.
# Using SubString instead of regex to avoid escaping the literal $ correctly.
$prefix = 'WireGuardTunnel$'
if ($ServiceName.StartsWith($prefix)) {
    $tunnelName = $ServiceName.Substring($prefix.Length)
} else {
    throw 'Only a WireGuardTunnel$ service may be removed'
}

Write-Host "Uninstalling tunnel: '$tunnelName' (service: '$ServiceName')"

# Never delete a service until SCM confirms it is stopped. Missing (1060) is
# success; access denied and other query errors are NOT evidence of removal.
sc.exe query $ServiceName 2>$null | Out-Null
if ($LASTEXITCODE -eq 1060) { Write-Output "UNINSTALLED $ServiceName"; exit 0 }
if ($LASTEXITCODE -ne 0) { throw "Cannot query service $ServiceName" }
$service = [System.ServiceProcess.ServiceController]::new($ServiceName)
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $service.Refresh()
        if ($service.Status -eq 'Stopped') { break }
        if ([DateTime]::UtcNow -ge $deadline) { throw "Service $ServiceName did not reach STOPPED" }
        if ($service.Status -ne 'StopPending' -and $service.Status -ne 'StartPending') { $service.Stop() }
        Start-Sleep -Milliseconds 200
    } while ($true)
} finally { $service.Dispose() }

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
        if ($LASTEXITCODE -eq 1060) { $removed = $true; break }
        if ($LASTEXITCODE -ne 0) { throw "Cannot confirm removal of $ServiceName" }
        Start-Sleep -Milliseconds 250
    }
}

if (-not $removed) {
    # Fallback: the service is already confirmed STOPPED above.
    sc.exe delete $ServiceName 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1060) { throw "Cannot delete $ServiceName" }
    for ($i = 0; $i -lt 20; $i++) {
        sc.exe query $ServiceName 2>$null | Out-Null
        if ($LASTEXITCODE -eq 1060) { $removed = $true; break }
        if ($LASTEXITCODE -ne 0) { throw "Cannot confirm removal of $ServiceName" }
        Start-Sleep -Milliseconds 250
    }
}

if (-not $removed) {
    Write-Error "Service $ServiceName still present after uninstall attempts"
    exit 1
}

Write-Output "UNINSTALLED $ServiceName"
exit 0
