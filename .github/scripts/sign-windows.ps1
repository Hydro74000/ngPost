param([Parameter(Mandatory=$true)][string]$File)
$ErrorActionPreference = 'Stop'
if ($env:REQUIRE_RELEASE_SIGNATURES -ne 'true') { exit 0 }
if (-not $env:WINDOWS_SIGNING_PFX -or -not $env:WINDOWS_SIGNING_PASSWORD) {
    throw 'Windows signing certificate and password are required for publication'
}
$pfx = Join-Path $env:RUNNER_TEMP ([guid]::NewGuid().ToString() + '.pfx')
$cert = $null
try {
    [IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:WINDOWS_SIGNING_PFX))
    $password = ConvertTo-SecureString $env:WINDOWS_SIGNING_PASSWORD -AsPlainText -Force
    $cert = Import-PfxCertificate -FilePath $pfx -CertStoreLocation Cert:\CurrentUser\My -Password $password | Where-Object HasPrivateKey | Select-Object -First 1
    if (-not $cert) { throw 'PFX contains no signing private key' }
    $tool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $tool) { throw 'signtool missing' }
    & $tool.FullName sign /fd SHA256 /td SHA256 /tr http://timestamp.digicert.com /s My /sha1 $cert.Thumbprint $File
    if ($LASTEXITCODE -ne 0) { throw 'Authenticode signing failed' }
    & $tool.FullName verify /pa $File
    if ($LASTEXITCODE -ne 0) { throw 'Authenticode verification failed' }
} finally {
    if ($cert) { Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -Force }
    Remove-Item -LiteralPath $pfx -Force -ErrorAction SilentlyContinue
}
