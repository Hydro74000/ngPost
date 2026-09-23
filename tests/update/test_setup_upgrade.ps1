# Native Inno Setup upgrade on a disposable GitHub runner only.
param(
    [Parameter(Mandatory = $true)][string]$PreviousSetup,
    [Parameter(Mandatory = $true)][string]$CandidateSetup
)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true') {
    throw 'This test installs/uninstalls the ngPost AppId. Use a disposable GitHub runner.'
}
$destination = Join-Path $env:RUNNER_TEMP 'ngPost setup upgrade'
$log = Join-Path $env:RUNNER_TEMP 'ngpost-setup-upgrade.log'
$arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-',
    '/TASKS=!desktopicon,installparpar,!installopenvpn,!installwireguard',
    "/DIR=`"$destination`"", "/LOG=`"$log`"")
try {
    foreach ($setup in @($PreviousSetup, $CandidateSetup)) {
        $process = Start-Process -FilePath $setup -ArgumentList $arguments -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw "Setup failed: $($process.ExitCode). See $log" }
        $output = & "$destination/ngPost.exe" --version 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0 -or $output -notmatch 'SSL support: yes') {
            throw "Installed binary failed: $output"
        }
        Write-Output $output
        if ($setup -eq $PreviousSetup) {
            Set-Content "$destination/ngPost.conf" 'CHECK_FOR_UPDATES = false' -Encoding ascii
        }
    }
    if (-not (Test-Path "$destination/.ngpost-installation")) { throw 'Missing ownership marker' }
    if (-not (Test-Path "$destination/unins000.exe")) { throw 'Uninstaller was lost' }
    if (-not (Test-Path "$destination/parpar.exe")) { throw 'Selected optional component was lost' }
    if ((Get-Content "$destination/ngPost.conf" -Raw).Trim() -ne 'CHECK_FOR_UPDATES = false') {
        throw 'Upgrade overwrote user configuration'
    }
    Write-Output 'PASS: 5.5.1 Setup -> candidate, TLS, ownership, uninstaller, optional tool and config'
}
finally {
    if (Test-Path "$destination/unins000.exe") {
        $uninstall = Start-Process "$destination/unins000.exe" -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
        if ($uninstall.ExitCode -ne 0) { throw "Uninstall failed: $($uninstall.ExitCode)" }
    }
}
