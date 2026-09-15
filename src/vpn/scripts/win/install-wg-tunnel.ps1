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
# Exit-code protocol consumed by VpnManager (RunAs cannot redirect stderr):
# 0 success; 2 WireGuard missing; 3 profile unreadable/invalid;
# 4 service installation/state failed; 5 profile rejected by wireguard.exe;
# 6 service ACL failed;
# 10 staging path unsafe/inaccessible; 1 other failure.

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
    Write-Error "wireguard.exe not found. Install WireGuard for Windows first." -ErrorAction Continue
    exit 2
}

if (-not (Test-Path -LiteralPath $ConfPath -PathType Leaf)) {
    Write-Error "Config file not found: $ConfPath" -ErrorAction Continue
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
        $line = ($raw -split '#', 2)[0].Trim()
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

# --- Step 0: create and verify an administrator-owned staging tree ------------
# A DACL alone is insufficient: an ordinary owner can grant itself access
# again, and a writable parent can replace a protected child. Refuse hostile
# existing paths instead of attempting to repair them in place.
function Assert-TrustedStagingPath {
    param([string] $Path, [switch] $Private)

    try {
        $item = Get-Item -LiteralPath $Path -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            # Read the tag with the system utility; no runtime compilation or
            # managed reflection into PowerShell internals. The tag is the first
            # hexadecimal value in fsutil's query output, independent of locale.
            $fsutil = Join-Path ([Environment]::SystemDirectory) 'fsutil.exe'
            $query = & $fsutil reparsepoint query $Path 2>&1
            $tagMatch = [regex]::Match(($query -join "`n"), '0x([0-9a-fA-F]{8})')
            if ($LASTEXITCODE -ne 0 -or -not $tagMatch.Success) {
                throw "cannot read the staging reparse point tag: $Path"
            }
            $tag = [Convert]::ToUInt32($tagMatch.Groups[1].Value, 16)
            if ($tag -band 0x20000000) {
                throw "staging path is a name-surrogate reparse point: $Path"
            }
        }
        $acl = Get-Acl -LiteralPath $Path
        $raw = [System.Security.AccessControl.RawSecurityDescriptor]::new(
            $acl.GetSecurityDescriptorBinaryForm(), 0)
        $trusted = @('S-1-5-18', 'S-1-5-32-544',
            'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
        if (-not $raw.Owner -or $trusted -notcontains $raw.Owner.Value) {
            throw ("staging path has an untrusted owner: $Path. Ask an administrator to inspect " +
                "this path and move aside the pre-created ngPost staging folder if it is not trusted, " +
                "then retry tunnel registration. Do not remove Windows system folders.")
        }
        if ($null -eq $raw.DiscretionaryAcl) { throw "staging path has no DACL: $Path" }
        # DELETE_CHILD, DELETE, WRITE_DAC, WRITE_OWNER, GENERIC_ALL. Creation of
        # siblings (FILE_ADD_FILE / FILE_ADD_SUBDIRECTORY) cannot replace this tree.
        $replacementRights = 0x100D0040
        foreach ($ace in $raw.DiscretionaryAcl) {
            if ([int]$ace.AceFlags -band [int][System.Security.AccessControl.AceFlags]::InheritOnly) { continue }
            if ($ace.AceType -eq [System.Security.AccessControl.AceType]::AccessDenied) { continue }
            if ($ace.AceType -ne [System.Security.AccessControl.AceType]::AccessAllowed) {
                throw "staging path has an unsupported access rule: $Path"
            }
            if ($trusted -contains $ace.SecurityIdentifier.Value) { continue }
            if ($Private -or ($ace.AccessMask -band $replacementRights)) {
                throw "staging path grants access to an untrusted account: $Path"
            }
        }
    } catch {
        # Preserve diagnostics for manual use, but expose a typed failure to
        # the entry point so it returns the stable staging code to ngPost.
        throw [System.Security.SecurityException]::new($_.Exception.Message, $_.Exception)
    }
}

function New-PrivateStagingDirectory {
    param([string] $Path, [switch] $Ancestor)

    # Windows PowerShell 5.1 uses .NET Framework: this overload supplies the
    # descriptor to CreateDirectoryW at creation, and leaves existing paths
    # untouched. Do not compile a helper with Add-Type in user-writable TEMP.
    $security = New-Object System.Security.AccessControl.DirectorySecurity
    $security.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)')
    [void][IO.Directory]::CreateDirectory($Path, $security)
    if (-not (Get-Item -LiteralPath $Path -Force).PSIsContainer) {
        throw "staging path is not a directory: $Path"
    }
    Assert-TrustedStagingPath -Path $Path -Private:(-not $Ancestor)
}

function New-ProtectedStagingDir {
    param([string] $BasePath = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::CommonApplicationData))

    # Use the Windows known folder rather than the caller's ProgramData
    # environment variable. Check existing ancestors before creating anything.
    $ancestors = @()
    $cursor = [IO.DirectoryInfo]::new($BasePath)
    while ($null -ne $cursor) {
        $ancestors = @($cursor.FullName) + $ancestors
        $cursor = $cursor.Parent
    }
    foreach ($path in $ancestors) { Assert-TrustedStagingPath -Path $path }
    $root = Join-Path $BasePath 'ngPost'
    $dir = Join-Path $root 'wg'
    New-PrivateStagingDirectory -Path $root -Ancestor
    New-PrivateStagingDirectory -Path $dir
    return $dir
}

function New-StagedWireGuardProfile {
    param([string] $ConfPath, [string] $Staging)

    Assert-TrustedStagingPath -Path $Staging -Private
    $destination = Join-Path $Staging ([IO.Path]::GetFileName($ConfPath))
    $temporary = Join-Path $Staging ([IO.Path]::GetRandomFileName())
    $created = $false
    try {
        # CreateNew refuses an existing file, including a preplanted link. A
        # bounded copy avoids allocating/copying an arbitrarily large source.
        $destinationStream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        $created = $true
        try {
            $sourceStream = [IO.File]::Open($ConfPath, [IO.FileMode]::Open,
                [IO.FileAccess]::Read, [IO.FileShare]::Read)
            try {
                $buffer = New-Object byte[] 65536
                $total = 0
                while (($count = $sourceStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    $total += $count
                    if ($total -gt 1048576) { throw 'the WireGuard profile is too large' }
                    $destinationStream.Write($buffer, 0, $count)
                }
            } finally { $sourceStream.Dispose() }
        } finally { $destinationStream.Dispose() }
        Assert-TrustedStagingPath -Path $temporary -Private
        Assert-WireGuardProfile -Path $temporary
        if (Test-Path -LiteralPath $destination) {
            Assert-TrustedStagingPath -Path $destination -Private
            # Only remove a verified file, never follow a destination link.
            [IO.File]::Delete($destination)
        }
        [IO.File]::Move($temporary, $destination)
        return $destination
    } finally {
        if ($created -and (Test-Path -LiteralPath $temporary)) { [IO.File]::Delete($temporary) }
    }
}

try {
    $staging = New-ProtectedStagingDir
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 10
}
# Preserve the basename: it determines the Windows tunnel service name.
try {
    $stagedConf = New-StagedWireGuardProfile -ConfPath $ConfPath -Staging $staging
} catch [System.Security.SecurityException] {
    Write-Error $_ -ErrorAction Continue
    exit 10
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 3
}

# Step 1: register the tunnel as a Windows service.
# Wait for the installer process itself: observing a freshly created STOPPED
# service is insufficient while wireguard.exe is still about to start it.
try {
    $installer = Start-Process -FilePath $wg -ArgumentList ('/installtunnelservice "' + $stagedConf + '"') -Wait -PassThru
    if ($installer.ExitCode -ne 0) {
        Write-Error "WireGuard rejected the profile (exit $($installer.ExitCode)). Check its key values and endpoint." -ErrorAction Continue
        exit 5
    }

    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($stagedConf)
    $svc = "WireGuardTunnel`$$baseName"

    $found = $false
    for ($i = 0; $i -lt 40; $i++) {
        sc.exe query $svc 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { $found = $true; break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $found) {
        throw "Service $svc did not appear after wireguard.exe /installtunnelservice. The .conf may be invalid."
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
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 4
}

# Step 2: extend the service ACL for the validated caller SID.
# Read the current SDDL and append an ACE granting SERVICE_START (RP) and
# SERVICE_STOP (WP) to the invoking user. sc.exe sdshow returns the whole
# SDDL on a single line (with leading blank line); collapse to one string
# and split D: from S: at the ")S:" boundary (a literal `S:` inside an ACE
# flag mnemonic like CCLCSW does NOT end the DACL, but the closing paren
# of the last DACL ACE does).
try {
    $raw = ((sc.exe sdshow $svc) -join '') -replace '\s', ''
    if ($LASTEXITCODE -ne 0) { throw "Cannot read service ACL for $svc" }
    if (-not $raw.StartsWith('D:')) {
        throw "Unexpected SDDL (missing DACL): $raw"
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
        throw "sc.exe sdset failed (exit $LASTEXITCODE) for SDDL: $newSddl"
    }
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 6
}

Write-Output "INSTALLED $svc for $InvokerSid"
exit 0
