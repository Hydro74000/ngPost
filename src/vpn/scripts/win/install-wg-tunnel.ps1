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
    [string]$InvokerSid
)

$ErrorActionPreference = "Stop"

# Validate before installing anything. No name lookup in the elevated account.
$sid = ([System.Security.Principal.SecurityIdentifier]::new($InvokerSid)).Value
if ($sid -notmatch '^S-1-5-21-\d+-\d+-\d+-\d+$' -and $sid -notmatch '^S-1-12-1-\d+-\d+-\d+-\d+$') {
    throw 'InvokerSid must identify a local/domain or Azure AD user'
}

function Find-Wireguard {
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
    foreach ($p in $candidates) {
        if (Test-Path -LiteralPath $p -PathType Leaf) { return $p }
    }
    return $null
}

$wg = Find-Wireguard
if (-not $wg) {
    Write-Error "wireguard.exe not found. Install WireGuard for Windows first."
    exit 2
}

if (-not (Test-Path -LiteralPath $ConfPath -PathType Leaf)) {
    Write-Error "Config file not found: $ConfPath"
    exit 3
}

# Step 1: register the tunnel as a Windows service.
# Wait for the installer process itself: observing a freshly created STOPPED
# service is insufficient while wireguard.exe is still about to start it.
$installer = Start-Process -FilePath $wg -ArgumentList ('/installtunnelservice "' + $ConfPath + '"') -Wait -PassThru
if ($installer.ExitCode -ne 0) { throw "WireGuard installation failed: $($installer.ExitCode)" }

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

# Import must leave a manual, stopped service, including when later ACL work
# fails. A successful sc stop is merely an accepted request, not completion.
try {
    sc.exe config $svc start= demand | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Cannot configure $svc as start=demand" }
    $startType = (Get-ItemProperty -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Services\$svc" -Name Start).Start
    if ($startType -ne 3) { throw "Service $svc is not start=demand" }
} finally {
    $service = [System.ServiceProcess.ServiceController]::new($svc)
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        do {
            $service.Refresh()
            if ($service.Status -eq 'Stopped') { break }
            if ([DateTime]::UtcNow -ge $deadline) { throw "Service $svc did not reach STOPPED" }
            if ($service.Status -ne 'StopPending' -and $service.Status -ne 'StartPending') { $service.Stop() }
            Start-Sleep -Milliseconds 200
        } while ($true)
    } finally { $service.Dispose() }
}

# Step 2: extend the service ACL for the validated caller SID.
# Read the current SDDL and append an ACE granting SERVICE_START (RP) and
# SERVICE_STOP (WP) to the invoking user. sc.exe sdshow returns the whole
# SDDL on a single line (with leading blank line); collapse to one string
# and split D: from S: at the ")S:" boundary (a literal `S:` inside an ACE
# flag mnemonic like CCLCSW does NOT end the DACL, but the closing paren
# of the last DACL ACE does).
$raw = ((sc.exe sdshow $svc) -join '') -replace '\s', ''
if ($LASTEXITCODE -ne 0) { throw "Cannot read service ACL for $svc" }
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
$newAce = "(A;;CCLCRPWP;;;$sid)" # query config/status, start, stop
if ($dPart -notlike "*$newAce*") {
    $dPart = $dPart + $newAce
}
$newSddl = $dPart + $sPart

sc.exe sdset $svc $newSddl
if ($LASTEXITCODE -ne 0) {
    Write-Error "sc.exe sdset failed (exit $LASTEXITCODE) for SDDL: $newSddl"
    exit $LASTEXITCODE
}

Write-Output "INSTALLED $svc for $InvokerSid"
exit 0
