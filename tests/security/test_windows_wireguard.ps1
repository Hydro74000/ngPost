# Native regression tests for the elevated installer, without creating services
# or bringing up a tunnel. Run with Windows PowerShell 5.1 as administrator.
$ErrorActionPreference = 'Stop'
$installer = Join-Path $PSScriptRoot '../../src/vpn/scripts/win/install-wg-tunnel.ps1'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path $installer).Path, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
# Load exactly the production policy/staging functions. Do not execute service
# installation or mock any ACL/file-system operations.
$productionFunctions = @()
foreach ($statement in $ast.EndBlock.Statements) {
    if ($statement -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $statement.Name -in @('Assert-WireGuardProfile', 'Assert-TrustedStagingPath',
            'New-PrivateStagingDirectory', 'New-ProtectedStagingDir', 'New-StagedWireGuardProfile')) {
        $productionFunctions += $statement.Extent.Text
        . ([scriptblock]::Create($statement.Extent.Text))
    }
    if ($statement -is [Management.Automation.Language.AssignmentStatementAst] -and
        $statement.Left.Extent.Text -in @('$WgInterfaceKeys', '$WgPeerKeys', '$WgDangerousKeys')) {
        $productionFunctions += $statement.Extent.Text
        . ([scriptblock]::Create($statement.Extent.Text))
    }
}

# Both elevated entry points must enforce the same path policy.
$uninstaller = Join-Path $PSScriptRoot '../../src/vpn/scripts/win/uninstall-wg-tunnel.ps1'
$uninstallAst = [Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path $uninstaller).Path, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$checker = @($uninstallAst.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $_.Name -eq 'Assert-TrustedStagingPath'
})
$installChecker = @($ast.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $_.Name -eq 'Assert-TrustedStagingPath'
})
if ($checker.Count -ne 1 -or $checker[0].Extent.Text -cne $installChecker[0].Extent.Text) {
    throw 'installer/uninstaller staging policies differ'
}

$cleanup = @($uninstallAst.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $_.Name -eq 'Remove-StagedWireGuardProfile'
})
if ($cleanup.Count -ne 1) { throw 'missing cleanup function' }
. ([scriptblock]::Create($cleanup[0].Extent.Text))

function Assert-Refused([scriptblock] $Action, [string] $Reason) {
    $failure = $null
    try { & $Action | Out-Null } catch { $failure = $_.Exception.Message }
    if (-not $failure -or $failure -notlike "*$Reason*") {
        throw "Expected refusal containing '$Reason', got '$failure'"
    }
}
function Set-TestSecurity([string] $Path, [string] $Sddl) {
    $acl = Get-Acl -LiteralPath $Path
    $acl.SetSecurityDescriptorSddlForm($Sddl)
    Set-Acl -LiteralPath $Path -AclObject $acl
}
function Test-Case([string] $Name, [scriptblock] $Action) {
    & $Action
    Write-Output "PASS: $Name"
}

# Run the production entry-point branches in child Windows PowerShell 5.1
# processes: testing exceptions alone would miss Write-Error swallowing exit N
# under ErrorActionPreference=Stop. Fixtures inherit the private test directory's
# ACL; stdout/stderr are captured by the test process, with no result file.
function Get-EntryStatement([string] $Pattern) {
    $matches = @($ast.EndBlock.Statements | Where-Object { $_.Extent.Text -match $Pattern })
    if ($matches.Count -ne 1) { throw "Expected one entry statement for $Pattern" }
    return $matches[0].Extent.Text
}
function Assert-EntryExit([int] $Expected, [string] $Body) {
    $fixture = Join-Path $base 'exit-probe.ps1'
    $process = New-Object Diagnostics.Process
    try {
        $preamble = @(
            '$ErrorActionPreference = "Stop"'
            ($productionFunctions -join "`n")
            ('$ConfPath = ''{0}''' -f $source.Replace("'", "''"))
            ('$staging = ''{0}''' -f $staging.Replace("'", "''"))
            ('$base = ''{0}''' -f $base.Replace("'", "''"))
            ('$sid = ''{0}''' -f $userSid)
            '$svc = "ngpost-test-no-real-service"'
        ) -join "`n"
        [IO.File]::WriteAllText($fixture, "$preamble`n$Body`nexit 0")
        $process.StartInfo.FileName = Join-Path ([Environment]::SystemDirectory) 'WindowsPowerShell\v1.0\powershell.exe'
        $process.StartInfo.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $fixture + '"'
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw 'Exit-code probe timed out'
        }
        if ($process.ExitCode -ne $Expected) {
            throw "Expected exit $Expected, got $($process.ExitCode): $($stdout.Result) $($stderr.Result)"
        }
    } finally {
        $process.Dispose()
        if (Test-Path -LiteralPath $fixture) { [IO.File]::Delete($fixture) }
    }
}

# Fail if a future change brings runtime compilation back into the elevated path.
function Add-Type { throw 'runtime compilation is forbidden in the staging tests' }

$base = Join-Path ([IO.Path]::GetPathRoot($env:SystemRoot)) ('ngpost-wg-test-' + [guid]::NewGuid())
$source = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName() + '.conf')
$adminAcl = 'O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)'
$userSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$profile = "[Interface] # interface = comment`nPrivateKey = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=`nAddress = 10.0.0.2/32`n[Peer] # peer`nPublicKey = AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=`nAllowedIPs = 0.0.0.0/0`n"
$link = $null
try {
    New-PrivateStagingDirectory -Path $base
    [IO.File]::WriteAllText($source, $profile)
    $staging = New-ProtectedStagingDir -BasePath $base

    Test-Case 'private directory creation does not need TEMP or a compiler' {
        $previousTemp = $env:TEMP
        $previousTmp = $env:TMP
        try {
            $env:TEMP = Join-Path $base 'missing-temp'
            $env:TMP = $env:TEMP
            $directory = Join-Path $base 'without-temp'
            New-PrivateStagingDirectory -Path $directory
            Assert-TrustedStagingPath -Path $directory -Private
            if (Test-Path $env:TEMP) { throw 'staging created a compiler temp directory' }
        } finally {
            $env:TEMP = $previousTemp
            $env:TMP = $previousTmp
        }
    }

    Test-Case 'fresh tree is private, reusable and preserves the profile basename' {
        $again = New-ProtectedStagingDir -BasePath $base
        if ($again -ne $staging) { throw 'staging changed' }
        $staged = New-StagedWireGuardProfile -ConfPath $source -Staging $staging
        if ([IO.Path]::GetFileName($staged) -ne [IO.Path]::GetFileName($source)) { throw 'basename changed' }
        if ([IO.File]::ReadAllText($staged) -ne $profile) { throw 'profile changed' }
        Assert-TrustedStagingPath -Path $staged -Private
    }
    Test-Case 'missing WireGuard returns exit 2 under ErrorActionPreference Stop' {
        Assert-EntryExit 2 ('$wg = $null' + "`n" + (Get-EntryStatement '^if \(-not \$wg\)'))
    }
    Test-Case 'missing profile returns exit 3 under ErrorActionPreference Stop' {
        Assert-EntryExit 3 ('$ConfPath = Join-Path $base "absent.conf"' + "`n" +
            (Get-EntryStatement '^if \(-not \(Test-Path -LiteralPath \$ConfPath'))
    }
    Test-Case 'untrusted staging root returns exit 10 without changing its owner' {
        $root = Join-Path $base 'ngPost'
        Set-TestSecurity $root ("O:${userSid}G:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)")
        try {
            $entry = (Get-EntryStatement '^try\s*\{\s*\$staging =').Replace(
                '$staging = New-ProtectedStagingDir', '$staging = New-ProtectedStagingDir -BasePath $base')
            Assert-EntryExit 10 $entry
            if ((Get-Acl $root).GetOwner([Security.Principal.SecurityIdentifier]).Value -ne $userSid) {
                throw 'exit-code handling changed the untrusted owner'
            }
        } finally { Set-TestSecurity $root $adminAcl }
    }
    Test-Case 'invalid profile returns exit 3 and preserves the installed copy' {
        [IO.File]::WriteAllText($source, "[Interface]`nPostUp = whoami`n")
        try {
            Assert-EntryExit 3 (Get-EntryStatement '^try\s*\{\s*\$stagedConf =')
            $destination = Join-Path $staging ([IO.Path]::GetFileName($source))
            if ([IO.File]::ReadAllText($destination) -ne $profile) { throw 'installed profile changed' }
        } finally { [IO.File]::WriteAllText($source, $profile) }
    }
    Test-Case 'untrusted staged destination returns exit 10 rather than profile error 3' {
        $destination = Join-Path $staging ([IO.Path]::GetFileName($source))
        Set-TestSecurity $destination ("O:${userSid}G:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)")
        try {
            Assert-EntryExit 10 (Get-EntryStatement '^try\s*\{\s*\$stagedConf =')
            if ([IO.File]::ReadAllText($destination) -ne $profile) { throw 'untrusted destination changed' }
        } finally { Set-TestSecurity $destination $adminAcl }
    }
    Test-Case 'service installer launch failure returns exit 4' {
        Assert-EntryExit 4 ('$wg = Join-Path $base "absent.exe"' + "`n" +
            (Get-EntryStatement '^try\s*\{\s*\$installer ='))
    }
    Test-Case 'service ACL read failure returns exit 6 regardless of native exit code' {
        Assert-EntryExit 6 ('function sc.exe { $global:LASTEXITCODE = 2 }' + "`n" +
            (Get-EntryStatement '^try\s*\{\s*\$raw ='))
    }
    Test-Case 'malformed service ACL returns exit 6' {
        Assert-EntryExit 6 ('function sc.exe { $global:LASTEXITCODE = 0; "invalid" }' + "`n" +
            (Get-EntryStatement '^try\s*\{\s*\$raw ='))
    }
    Test-Case 'service ACL write failure returns exit 6 regardless of native exit code' {
        Assert-EntryExit 6 ('function sc.exe { if ($args[0] -eq "sdshow") { $global:LASTEXITCODE = 0; "D:(A;;CC;;;SY)" } else { $global:LASTEXITCODE = 3 } }' + "`n" +
            (Get-EntryStatement '^try\s*\{\s*\$raw ='))
    }
    Test-Case 'creation-only rights on an ancestor do not allow replacing protected children' {
        Set-TestSecurity $base ($adminAcl + '(A;;0x6;;;AU)')
        New-ProtectedStagingDir -BasePath $base | Out-Null
        Set-TestSecurity $base $adminAcl
    }
    Test-Case 'legacy admin-owned parent with inherited read access remains usable' {
        $root = Join-Path $base 'ngPost'
        Set-TestSecurity $root ($adminAcl + '(A;;FR;;;BU)')
        New-ProtectedStagingDir -BasePath $base | Out-Null
        Set-TestSecurity $root $adminAcl
    }
    Test-Case 'an ordinary owner is refused without repairing its permissions' {
        $root = Join-Path $base 'ngPost'
        Set-TestSecurity $root ("O:${userSid}G:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)")
        Assert-Refused { New-ProtectedStagingDir -BasePath $base } 'untrusted owner'
        Assert-Refused { New-ProtectedStagingDir -BasePath $base } 'Ask an administrator'
        if ((Get-Acl $root).GetOwner([Security.Principal.SecurityIdentifier]).Value -ne $userSid) {
            throw 'untrusted existing root was modified'
        }
        Set-TestSecurity $root $adminAcl
    }
    Test-Case 'DELETE_CHILD on the parent is refused' {
        Set-TestSecurity $base ($adminAcl + '(A;;0x40;;;AU)')
        Assert-Refused { New-ProtectedStagingDir -BasePath $base } 'untrusted account'
        Set-TestSecurity $base $adminAcl
    }
    Test-Case 'a precreated user-owned wg directory is refused' {
        Set-TestSecurity $staging ("O:${userSid}G:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)")
        Assert-Refused { New-ProtectedStagingDir -BasePath $base } 'untrusted owner'
        Set-TestSecurity $staging $adminAcl
    }
    Test-Case 'junction ancestors are refused' {
        $script:link = Join-Path $base 'junction'
        cmd /c mklink /J $script:link $staging | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'cannot create test junction' }
        Assert-Refused { New-ProtectedStagingDir -BasePath $script:link } 'reparse point'
        cmd /c rmdir $script:link
        $script:link = $null
    }
    Test-Case 'script keys with inline comments remain forbidden and preserve the installed copy' {
        $destination = Join-Path $staging ([IO.Path]::GetFileName($source))
        foreach ($key in @('PreUp', 'PostUp', 'PreDown', 'PostDown')) {
            [IO.File]::WriteAllText($source, "[Interface]`n$key = whoami # comment`n")
            Assert-Refused { New-StagedWireGuardProfile -ConfPath $source -Staging $staging } 'runs a command'
        }
        if ([IO.File]::ReadAllText($destination) -ne $profile) { throw 'old profile was replaced' }
    }
    Test-Case 'WOF compression is accepted while untrusted write permissions are refused' {
        $compressed = Join-Path $staging 'compressed.conf'
        [IO.File]::WriteAllText($compressed, $profile + ("# padding`n" * 16384))
        $compact = Join-Path ([Environment]::SystemDirectory) 'compact.exe'
        & $compact /C /EXE:XPRESS4K /F $compressed | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'compact failed' }
        # WOF may hide its reparse attribute through its file-system filter.
        # Exercise the compressed file regardless of that implementation detail.
        Assert-TrustedStagingPath -Path $compressed -Private
        Set-TestSecurity $compressed ($adminAcl + '(A;;FW;;;AU)')
        Assert-Refused { Assert-TrustedStagingPath -Path $compressed -Private } 'untrusted account'
        [IO.File]::Delete($compressed)
    }
    Test-Case 'oversized sources are refused before promotion' {
        [IO.File]::WriteAllBytes($source, (New-Object byte[] 1048577))
        Assert-Refused { New-StagedWireGuardProfile -ConfPath $source -Staging $staging } 'too large'
    }
    Test-Case 'a preexisting untrusted destination cannot be overwritten' {
        $destination = Join-Path $staging ([IO.Path]::GetFileName($source))
        Set-TestSecurity $destination ("O:${userSid}G:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)")
        [IO.File]::WriteAllText($source, $profile)
        Assert-Refused { New-StagedWireGuardProfile -ConfPath $source -Staging $staging } 'untrusted owner'
        Set-TestSecurity $destination $adminAcl
    }
    Test-Case 'failed validations leave no temporary copies' {
        if (@(Get-ChildItem -LiteralPath $staging -Force).Count -ne 1) { throw 'temporary files leaked' }
    }
    Test-Case 'cleanup refuses an untrusted tree without deleting the profile' {
        $name = [IO.Path]::GetFileNameWithoutExtension($source)
        Set-TestSecurity $base ($adminAcl + '(A;;0x40;;;AU)')
        Assert-Refused { Remove-StagedWireGuardProfile -TunnelName $name -BasePath $base } 'untrusted account'
        if (-not (Test-Path (Join-Path $staging ([IO.Path]::GetFileName($source))))) {
            throw 'profile was removed through an untrusted tree'
        }
        Set-TestSecurity $base $adminAcl
    }
    Test-Case 'cleanup removes only the requested tunnel and tolerates its absence' {
        $name = [IO.Path]::GetFileNameWithoutExtension($source)
        $other = Join-Path $staging 'other.conf'
        [IO.File]::WriteAllText($other, $profile)
        Remove-StagedWireGuardProfile -TunnelName $name -BasePath $base
        Remove-StagedWireGuardProfile -TunnelName $name -BasePath $base
        if (@(Get-ChildItem -LiteralPath $staging).Count -ne 1 -or -not (Test-Path $other)) {
            throw 'cleanup changed an unrelated profile'
        }
    }

} finally {
    if ($link) { cmd /c rmdir $link }
    if (Test-Path -LiteralPath $base) { Remove-Item -LiteralPath $base -Recurse -Force }
    if (Test-Path -LiteralPath $source) { Remove-Item -LiteralPath $source -Force }
}
