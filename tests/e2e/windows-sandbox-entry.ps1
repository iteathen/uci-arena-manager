[CmdletBinding()]
param(
    [string]$MappedRoot = "C:\SuiteE2E"
)

$ErrorActionPreference = "Stop"
$exitCode = 1

try {
    $localMedia = "C:\SuiteE2EMedia"
    New-Item -ItemType Directory -Force -Path $localMedia | Out-Null
    Copy-Item -LiteralPath (Join-Path $MappedRoot "bootstrap") `
        -Destination (Join-Path $localMedia "bootstrap") -Recurse
    Copy-Item -LiteralPath (Join-Path $MappedRoot "suite") `
        -Destination (Join-Path $localMedia "suite") -Recurse
    & (Join-Path $MappedRoot "windows-complete-suite.ps1") `
        -Setup (Join-Path $localMedia "bootstrap\bin\arena-setup.exe") `
        -Catalog (Join-Path $localMedia "suite\catalog.json") `
        -RunRoot "C:\ArenaE2E" `
        -ResultPath (Join-Path $MappedRoot "result.json") `
        -AllowUnsignedDevelopmentCatalog
    $exitCode = 0
} catch {
    Write-Error $_
} finally {
    $status = @{
        schema = "arena_windows_sandbox_result_v1"
        exit_code = $exitCode
        completed_at = [DateTimeOffset]::UtcNow.ToString("O")
    }
    [IO.File]::WriteAllText(
        (Join-Path $MappedRoot "sandbox-status.json"),
        ($status | ConvertTo-Json) + [Environment]::NewLine
    )
    Start-Sleep -Seconds 2
    & "$env:SystemRoot\System32\shutdown.exe" /s /t 0 /f
}

exit $exitCode
