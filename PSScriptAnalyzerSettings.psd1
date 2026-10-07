# PSScriptAnalyzer rules the CI enforces on the PowerShell scripts
# (unit-tests.yml, Windows job). Errors and warnings fail the job.
@{
    Severity     = @('Error', 'Warning')
    ExcludeRules = @(
        # -WhatIf/-Confirm support is for interactive cmdlets. These helpers run
        # inside scripts started elevated and unattended by ngPost or by CI,
        # where nobody could answer a confirmation.
        'PSUseShouldProcessForStateChangingFunctions',
        # Write-Host goes to the information stream since PowerShell 5. The
        # elevated scripts report to ngPost through exit codes, and the CI
        # scripts write progress for a human reading the log.
        'PSAvoidUsingWriteHost'
    )
}
