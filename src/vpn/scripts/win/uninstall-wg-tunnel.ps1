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

# The staged copy install-wg-tunnel.ps1 validated and installed from. Removed
# only now, with the service confirmed gone, so a failed uninstall never leaves
# a registered tunnel whose staged profile has been deleted under it.
#
# Absence is normal and not an error: a tunnel registered by an ngPost older
# than the staging step has no copy here. Failing to delete one is not worth
# failing the uninstall either -- the service is gone, which is what was asked.
# Kept identical to the installer and checked by test_windows_wireguard.ps1.
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

function Remove-StagedWireGuardProfile {
    param([string] $TunnelName, [string] $BasePath = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::CommonApplicationData))

    $staging = Join-Path (Join-Path $BasePath 'ngPost') 'wg'
    if (Test-Path -LiteralPath $staging) {
        $ancestors = @()
        $cursor = [IO.DirectoryInfo]::new($staging)
        while ($null -ne $cursor) {
            $ancestors = @($cursor.FullName) + $ancestors
            $cursor = $cursor.Parent
        }
        foreach ($path in $ancestors) { Assert-TrustedStagingPath -Path $path }
        Assert-TrustedStagingPath -Path $staging -Private
        Get-ChildItem -LiteralPath $staging -File -Force |
            Where-Object { [IO.Path]::GetFileNameWithoutExtension($_.Name) -eq $TunnelName } |
            ForEach-Object {
                Assert-TrustedStagingPath -Path $_.FullName -Private
                [IO.File]::Delete($_.FullName)
            }
    }
}
try {
    Remove-StagedWireGuardProfile -TunnelName $tunnelName
} catch {
    Write-Host "Could not safely remove the staged profile: $($_.Exception.Message)"
}

Write-Output "UNINSTALLED $ServiceName"
exit 0
