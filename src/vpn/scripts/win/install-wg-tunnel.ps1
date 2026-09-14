# install-wg-tunnel.ps1 — register a WireGuard tunnel service for ngPost.
#
# Called by ngPost (via UAC elevation) when the user creates or edits a
# WireGuard profile on Windows. The work happens in three steps:
#   0. stage + validate — the profile is copied into an administrators-only
#      folder and the COPY is validated and installed. ngPost validates the
#      original too, but it lives in the user's own configuration folder: any
#      process running as that user can rewrite it between that check and this
#      one. Validating a file we then hand to an elevated wireguard.exe is only
#      meaningful if the file can no longer change, which is what the copy
#      buys. Same reasoning as the Linux helper, which sanitises its own copy
#      as root instead of trusting the caller's file.
#   1. wireguard.exe /installtunnelservice <staged conf>  — creates a Windows
#      service named "WireGuardTunnel$<basename-of-conf>" running as SYSTEM.
#      The basename is preserved by the copy, so the service name is unchanged.
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

# --- WireGuard profile policy -------------------------------------------------
# Mirrors WireGuardConfigPolicy (C++) and sanitize_wireguard_profile() in the
# Linux helper. tst_WireGuardConfigPolicy reads all three and fails if the key
# lists drift apart.
#
# The four script keys are REFUSED here, not stripped: wireguard.exe reads the
# file we hand it, so what cannot be removed must be rejected. A key that is not
# one of ours is never echoed back -- the profile may be any file, and quoting a
# line of it would turn this refusal into a way to read it.
$WgInterfaceKeys = @(
    'address','addresses','dns','fwmark','listenport','mtu',
    'postdown','postup','predown','preup',
    'privatekey','saveconfig','table'
)
$WgPeerKeys      = @('allowedips','endpoint','persistentkeepalive','presharedkey','publickey')
$WgDangerousKeys = @('postdown','postup','predown','preup')

function Assert-WireGuardProfile {
    param([Parameter(Mandatory=$true)][string] $Path)

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -gt 1048576) { throw 'the WireGuard profile is larger than a profile ever is' }
    if ($bytes -contains 0)        { throw 'the WireGuard profile contains binary data' }

    $section = ''
    $lineNo  = 0
    foreach ($raw in [System.IO.File]::ReadAllLines($Path)) {
        $lineNo++
        $line = $raw.Trim()
        if ($line -eq '' -or $line.StartsWith('#') -or $line.StartsWith(';')) { continue }

        if ($line.StartsWith('[')) {
            if (-not $line.EndsWith(']')) { throw "malformed section header on line $lineNo" }
            $name = $line.Substring(1, $line.Length - 2).Trim().ToLowerInvariant()
            if ($name -eq 'interface')  { $section = 'interface'; continue }
            if ($name -eq 'peer')       { $section = 'peer';      continue }
            throw "only [Interface] and [Peer] sections are allowed (line $lineNo)"
        }

        $sep = $line.IndexOf('=')
        if ($sep -lt 1) { throw "expected a Key = Value line (line $lineNo)" }
        $key = $line.Substring(0, $sep).Trim().ToLowerInvariant()
        if ($key -eq '') { throw "expected a Key = Value line (line $lineNo)" }

        $knownInterface = $WgInterfaceKeys -contains $key
        $knownPeer      = $WgPeerKeys      -contains $key
        if ($section -eq '') {
            throw "a key appears before any [Interface] or [Peer] section (line $lineNo)"
        }
        if ($WgDangerousKeys -contains $key) {
            throw ("'" + $key + "' runs a command when the tunnel goes up or down, which ngPost " +
                   "never needs. Remove that line from the profile (line $lineNo).")
        }
        $fits = if ($section -eq 'interface') { $knownInterface } else { $knownPeer }
        if (-not $fits) {
            if ($knownInterface -or $knownPeer) {
                throw ("'" + $key + "' does not belong to this section (line $lineNo)")
            }
            throw "this profile carries a key ngPost has not reviewed (line $lineNo)"
        }
    }
}

# --- Step 0: stage the profile where only administrators can write it ---------
function New-ProtectedStagingDir {
    $root = Join-Path $env:ProgramData 'ngPost'
    $dir  = Join-Path $root 'wg'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    $system = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-18')
    $admins = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-544')
    $acl = Get-Acl -LiteralPath $dir
    $acl.SetAccessRuleProtection($true, $false)   # protected: drop every inherited ACE
    # Best-effort, deliberately: RemoveAccessRule throws on an entry .NET
    # considers inherited, and with $ErrorActionPreference='Stop' that would
    # abort a legitimate installation. The read-back below is the guarantee, not
    # this loop -- so a failure to remove is only worth continuing past.
    foreach ($rule in @($acl.Access)) {
        try { [void]$acl.RemoveAccessRule($rule) } catch { }
    }
    foreach ($sid in @($system, $admins)) {
        $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(
            $sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')))
    }
    Set-Acl -LiteralPath $dir -AclObject $acl

    # Read it back. A Set-Acl that reports success while an ACE survives would
    # leave the staged copy as rewritable as the original, which is the one
    # thing this folder exists to prevent.
    $trusted = @($system.Value, $admins.Value)
    foreach ($ace in (Get-Acl -LiteralPath $dir).Access) {
        if ($ace.AccessControlType -ne 'Allow') { continue }
        if (($ace.FileSystemRights -band ([System.Security.AccessControl.FileSystemRights]::Write -bor
                                          [System.Security.AccessControl.FileSystemRights]::Modify -bor
                                          [System.Security.AccessControl.FileSystemRights]::FullControl -bor
                                          [System.Security.AccessControl.FileSystemRights]::Delete)) -eq 0) { continue }
        $sid = $null
        try {
            $sid = $ace.IdentityReference.Translate(
                [System.Security.Principal.SecurityIdentifier]).Value
        } catch { }
        # An identity we cannot resolve is not evidence of safety. Fail closed
        # with a message that says which folder, rather than letting an opaque
        # .NET exception surface as an unexplained exit code.
        if (-not $sid) {
            throw "the staging folder $dir grants write access to an unresolvable account"
        }
        if ($trusted -notcontains $sid) {
            throw "the staging folder $dir still grants write access to $sid"
        }
    }
    return $dir
}

$staging = New-ProtectedStagingDir
# Same basename: wireguard.exe derives the service name from it, and ngPost
# computes the same name on its side from the original path.
$stagedConf = Join-Path $staging ([System.IO.Path]::GetFileName($ConfPath))
Copy-Item -LiteralPath $ConfPath -Destination $stagedConf -Force
Assert-WireGuardProfile -Path $stagedConf

# Step 1: register the tunnel as a Windows service.
# Wait for the installer process itself: observing a freshly created STOPPED
# service is insufficient while wireguard.exe is still about to start it.
$installer = Start-Process -FilePath $wg -ArgumentList ('/installtunnelservice "' + $stagedConf + '"') -Wait -PassThru
if ($installer.ExitCode -ne 0) { throw "WireGuard installation failed: $($installer.ExitCode)" }

$baseName = [System.IO.Path]::GetFileNameWithoutExtension($stagedConf)
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
