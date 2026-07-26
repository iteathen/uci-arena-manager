[CmdletBinding()]
param(
    [ValidateRange(1, 65535)]
    [int]$Port = 47831,

    [switch]$NoOpen,

    [switch]$Stop
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$releaseDirectory = Join-Path $repositoryRoot 'build\windows-release\Release'
$serviceExecutable = Join-Path $releaseDirectory 'arena-service.exe'
$cliExecutable = Join-Path $releaseDirectory 'arena.exe'
$webRoot = Join-Path $repositoryRoot 'web\dist'
$dataDirectory = Join-Path $repositoryRoot 'runtime'
$pidFile = Join-Path $dataDirectory "local-service-$Port.pid"
$stdoutLog = Join-Path $dataDirectory "local-service-$Port.out.log"
$stderrLog = Join-Path $dataDirectory "local-service-$Port.err.log"
$endpoint = "http://127.0.0.1:$Port"

function Assert-Artifact {
    param(
        [string]$Path,
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description is missing at '$Path'. Build from '$repositoryRoot' with: cmake --build --preset windows-release; npm run build --prefix web"
    }
}

function Test-ArenaReady {
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $cliExecutable `
            --endpoint $endpoint `
            --data-dir $dataDirectory `
            --output json `
            status 1> $null 2> $null
        $readyExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    return $readyExitCode -eq 0
}

function Get-TrackedProcess {
    if (-not (Test-Path -LiteralPath $pidFile)) {
        return $null
    }

    $processId = 0
    if (-not [int]::TryParse(
            (Get-Content -LiteralPath $pidFile -Raw).Trim(),
            [ref]$processId)) {
        return $null
    }

    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        return $null
    }

    try {
        $actualPath = [System.IO.Path]::GetFullPath($process.Path)
    } catch {
        return $null
    }
    if (-not $actualPath.Equals(
            [System.IO.Path]::GetFullPath($serviceExecutable),
            [System.StringComparison]::OrdinalIgnoreCase)) {
        return $null
    }
    return $process
}

if ($Stop) {
    $trackedProcess = Get-TrackedProcess
    if ($null -eq $trackedProcess) {
        Write-Host "No launcher-owned arena service is running on port $Port."
    } else {
        Stop-Process -Id $trackedProcess.Id
        Wait-Process -Id $trackedProcess.Id -Timeout 10 -ErrorAction SilentlyContinue
        Write-Host "Stopped UCI Arena Manager process $($trackedProcess.Id)."
    }
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
    exit 0
}

Assert-Artifact -Path $serviceExecutable -Description 'arena-service.exe'
Assert-Artifact -Path $cliExecutable -Description 'arena.exe'
Assert-Artifact -Path (Join-Path $webRoot 'index.html') -Description 'dashboard assets'
New-Item -ItemType Directory -Force -Path $dataDirectory | Out-Null

$startedProcess = $null
if (-not (Test-ArenaReady)) {
    $quote = {
        param([string]$Value)
        return '"' + $Value.Replace('"', '\"') + '"'
    }
    $serviceArguments = @(
        '--data-dir',
        (& $quote $dataDirectory),
        '--web-root',
        (& $quote $webRoot),
        '--port',
        $Port
    )
    $startedProcess = Start-Process `
        -FilePath $serviceExecutable `
        -ArgumentList $serviceArguments `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog `
        -WindowStyle Hidden `
        -PassThru
    Set-Content -LiteralPath $pidFile -Value $startedProcess.Id -Encoding ascii

    $ready = $false
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        if ($startedProcess.HasExited) {
            break
        }
        Start-Sleep -Milliseconds 100
        if (Test-ArenaReady) {
            $ready = $true
            break
        }
    }
    if (-not $ready) {
        if (-not $startedProcess.HasExited) {
            Stop-Process -Id $startedProcess.Id -ErrorAction SilentlyContinue
        }
        Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
        throw "UCI Arena Manager did not become ready. Inspect '$stderrLog' and '$stdoutLog'."
    }
    Write-Host "Started UCI Arena Manager on $endpoint (PID $($startedProcess.Id))."
} else {
    Write-Host "Reusing UCI Arena Manager already running on $endpoint."
}

Write-Host "Stop it later with: `"$repositoryRoot\run-local.cmd`" -Stop -Port $Port"
$uiArguments = @(
    '--endpoint',
    $endpoint,
    '--data-dir',
    $dataDirectory,
    'ui'
)
if ($NoOpen) {
    $uiArguments += '--no-open'
}
& $cliExecutable @uiArguments
exit $LASTEXITCODE
