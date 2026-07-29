[CmdletBinding()]
param(
    [ValidateSet("prepare", "install", "adopt", "smoke", "status", "restore")]
    [string]$Action = "status",
    [string]$Preset = "windows-release",
    [string]$StatePath = "",
    [string]$MsiPath = "",
    [string]$CandidateSourceCommit = "",
    [string]$WixExe = "",
    [string]$DataRoot = (Join-Path $env:ProgramData "UCI Arena Manager"),
    [string]$InstallRoot = (Join-Path $env:ProgramFiles "UCI Arena Manager"),
    [string]$QuarantinePath = "",
    [string]$LichessTokenFile = "",
    [string[]]$ProbeEngine = @(),
    [switch]$SkipBuild,
    [switch]$RequirePlanningEvidence,
    [switch]$RestartService,
    [switch]$ConfirmAdopt,
    [switch]$ConfirmRestore
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$script:CycleSchema = "arena_windows_installer_cycle_v1"

function Get-UtcTimestamp {
    return [DateTime]::UtcNow.ToString("o")
}

function Resolve-FullPath {
    param([Parameter(Mandatory)][string]$Path)
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $script:RepositoryRoot $Path))
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @(),
        [switch]$Capture
    )
    if ($Capture) {
        $lines = @(& $FilePath @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0) {
            $detail = ($lines | Select-Object -Last 4) -join [Environment]::NewLine
            throw "$FilePath failed with exit code $exitCode`n$detail"
        }
        return $lines -join [Environment]::NewLine
    }
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Assert-Windows {
    if ($env:OS -ne "Windows_NT") {
        throw "Windows installer-cycle actions require Windows"
    }
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-Administrator {
    if (-not (Test-IsAdministrator)) {
        throw "Action '$Action' requires an elevated PowerShell session"
    }
}

function Save-Cycle {
    param([Parameter(Mandatory)][System.Collections.IDictionary]$Cycle)
    $directory = Split-Path -Parent $script:StatePath
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $temporary = "$($script:StatePath).tmp"
    $json = $Cycle | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText($temporary, $json + [Environment]::NewLine)
    Move-Item -LiteralPath $temporary -Destination $script:StatePath -Force
}

function Read-Cycle {
    if (-not (Test-Path -LiteralPath $script:StatePath -PathType Leaf)) {
        throw "Installer-cycle state does not exist: $($script:StatePath)"
    }
    $value = Get-Content -Raw -LiteralPath $script:StatePath | ConvertFrom-Json -AsHashtable
    if ($value.schema -ne $script:CycleSchema) {
        throw "Unsupported installer-cycle state schema"
    }
    return $value
}

function Get-SourceCommit {
    return (Invoke-Checked git @("-C", $script:RepositoryRoot, "rev-parse", "HEAD") -Capture).Trim()
}

function Assert-CleanTrackedTree {
    $status = (Invoke-Checked git @(
        "-C", $script:RepositoryRoot, "status", "--porcelain=v1", "--untracked-files=no"
    ) -Capture).Trim()
    if ($status) {
        throw "Tracked worktree changes must be committed before preparing or adopting a candidate"
    }
}

function Test-SensitiveRelativePath {
    param([Parameter(Mandatory)][string]$RelativePath)
    return $RelativePath -match "(?i)(^|/)(client\.key|[^/]*(token|secret|credential)[^/]*)$"
}

function New-RetainedManifest {
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$OutputPath
    )
    $rootPath = Resolve-FullPath $Root
    $entries = @()
    if (Test-Path -LiteralPath $rootPath -PathType Container) {
        foreach ($file in Get-ChildItem -LiteralPath $rootPath -Recurse -File -Force |
            Sort-Object FullName) {
            $relative = [IO.Path]::GetRelativePath($rootPath, $file.FullName).Replace("\", "/")
            $sensitive = Test-SensitiveRelativePath $relative
            $entries += [ordered]@{
                relative_path = $relative
                size_bytes = $file.Length
                last_write_time_utc = $file.LastWriteTimeUtc.ToString("o")
                sensitive = $sensitive
                sha256 = if ($sensitive) { $null } else {
                    (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
                }
            }
        }
    }
    [int64]$totalBytes = 0
    foreach ($entry in $entries) {
        $totalBytes += [int64]$entry["size_bytes"]
    }
    $manifest = [ordered]@{
        schema = "arena_retained_state_manifest_v1"
        recorded_at_utc = Get-UtcTimestamp
        root = $rootPath
        existed = Test-Path -LiteralPath $rootPath -PathType Container
        root_acl_sddl = if (Test-Path -LiteralPath $rootPath -PathType Container) {
            (Get-Acl -LiteralPath $rootPath).Sddl
        } else { $null }
        file_count = $entries.Count
        total_bytes = $totalBytes
        files = $entries
    }
    $parent = Split-Path -Parent $OutputPath
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    [IO.File]::WriteAllText(
        $OutputPath,
        ($manifest | ConvertTo-Json -Depth 10) + [Environment]::NewLine
    )
    return $manifest
}

function Assert-RetainedManifest {
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$ManifestPath
    )
    $expected = Get-Content -Raw -LiteralPath $ManifestPath |
        ConvertFrom-Json -AsHashtable
    $comparisonPath = Join-Path (Split-Path -Parent $ManifestPath) "restore-comparison.json"
    $actual = New-RetainedManifest -Root $Root -OutputPath $comparisonPath
    if ($expected.existed -ne $actual.existed -or
        [int64]$expected.file_count -ne [int64]$actual.file_count -or
        [int64]$expected.total_bytes -ne [int64]$actual.total_bytes -or
        $expected.root_acl_sddl -ne $actual.root_acl_sddl) {
        throw "Retained-state root metadata changed"
    }
    $actualByPath = @{}
    foreach ($entry in $actual.files) {
        $actualByPath[$entry.relative_path] = $entry
    }
    foreach ($entry in $expected.files) {
        if (-not $actualByPath.ContainsKey($entry.relative_path)) {
            throw "Retained-state file is missing: $($entry.relative_path)"
        }
        $observed = $actualByPath[$entry.relative_path]
        if ([int64]$entry.size_bytes -ne [int64]$observed.size_bytes -or
            $entry.last_write_time_utc -ne $observed.last_write_time_utc -or
            $entry.sha256 -ne $observed.sha256) {
            throw "Retained-state file changed: $($entry.relative_path)"
        }
    }
}

function Assert-SafeSiblingPaths {
    param(
        [Parameter(Mandatory)][string]$Canonical,
        [Parameter(Mandatory)][string]$Sibling
    )
    $canonicalPath = Resolve-FullPath $Canonical
    $siblingPath = Resolve-FullPath $Sibling
    if ($canonicalPath -eq [IO.Path]::GetPathRoot($canonicalPath) -or
        $siblingPath -eq [IO.Path]::GetPathRoot($siblingPath)) {
        throw "A filesystem root cannot be used by an installer cycle"
    }
    if ((Split-Path -Parent $canonicalPath) -ne (Split-Path -Parent $siblingPath) -or
        [IO.Path]::GetPathRoot($canonicalPath) -ne [IO.Path]::GetPathRoot($siblingPath) -or
        $canonicalPath -eq $siblingPath) {
        throw "The quarantine must be a distinct same-volume sibling of the data root"
    }
}

function Get-HttpStatusCode {
    param(
        [Parameter(Mandatory)][string]$Uri,
        [hashtable]$Headers = @{}
    )
    try {
        return (Invoke-WebRequest -Uri $Uri -Headers $Headers -UseBasicParsing -TimeoutSec 10).StatusCode
    } catch {
        if ($null -ne $_.Exception.Response) {
            return [int]$_.Exception.Response.StatusCode
        }
        return -1
    }
}

function Write-SafeSummary {
    param([Parameter(Mandatory)][System.Collections.IDictionary]$Cycle)
    [ordered]@{
        schema = $Cycle.schema
        cycle_id = $Cycle.cycle_id
        phase = $Cycle.phase
        source_commit = $Cycle.source_commit
        runner_commit = $Cycle.runner_commit
        candidate_sha256 = $Cycle.candidate.sha256
        install_invocation_count = $Cycle.install.invocation_count
        smoke_runs = $Cycle.smoke.runs
        state_path = $script:StatePath
        quarantine_path = $Cycle.paths.quarantine
    } | ConvertTo-Json -Compress
}

Assert-Windows
$sourceCommit = Get-SourceCommit
if (-not $StatePath) {
    $StatePath = Join-Path $script:RepositoryRoot (
        "build\installer-validation\{0}\cycle.json" -f $sourceCommit.Substring(0, 12)
    )
}
$script:StatePath = Resolve-FullPath $StatePath
$DataRoot = Resolve-FullPath $DataRoot
$InstallRoot = Resolve-FullPath $InstallRoot

if ($Action -eq "prepare") {
    Assert-CleanTrackedTree
    if ($CandidateSourceCommit -and $CandidateSourceCommit -ne $sourceCommit) {
        throw "Prepare builds from the checked-out commit; use adopt for an older candidate"
    }
    if (Test-Path -LiteralPath $script:StatePath) {
        throw "Cycle state already exists; use status, install, or smoke instead of rebuilding"
    }
    if ($WixExe) {
        $resolvedWix = Resolve-FullPath $WixExe
        if (-not (Test-Path -LiteralPath $resolvedWix -PathType Leaf)) {
            throw "WiX executable does not exist: $resolvedWix"
        }
        $env:PATH = "$(Split-Path -Parent $resolvedWix);$env:PATH"
    }
    if (-not $SkipBuild) {
        Push-Location $script:RepositoryRoot
        try {
            Invoke-Checked node @("tools/spec-check.mjs")
            Invoke-Checked node @("--test", "tools/release/*.test.mjs")
            Invoke-Checked npm @("test", "--prefix", "web")
            Invoke-Checked npm @("run", "build", "--prefix", "web")
            Invoke-Checked cmake @("--preset", $Preset)
            Invoke-Checked cmake @("--build", "--preset", $Preset)
            Invoke-Checked ctest @("--preset", $Preset)
            Push-Location (Join-Path $script:RepositoryRoot "build\$Preset")
            try {
                Invoke-Checked cpack @("-C", "Release", "-G", "WIX")
            } finally {
                Pop-Location
            }
        } finally {
            Pop-Location
        }
    }
    if (-not $MsiPath) {
        $candidates = @(Get-ChildItem -LiteralPath (
            Join-Path $script:RepositoryRoot "build\$Preset"
        ) -File -Filter "*.msi")
        if ($candidates.Count -ne 1) {
            throw "Expected exactly one MSI candidate; found $($candidates.Count)"
        }
        $MsiPath = $candidates[0].FullName
    }
    $MsiPath = Resolve-FullPath $MsiPath
    if (-not (Test-Path -LiteralPath $MsiPath -PathType Leaf)) {
        throw "MSI candidate does not exist: $MsiPath"
    }
    $cycleId = "$($sourceCommit.Substring(0, 12))-$(Get-Date -AsUTC -Format 'yyyyMMdd-HHmmss')"
    if (-not $QuarantinePath) {
        $QuarantinePath = "$DataRoot.pre-clean-install-$cycleId"
    }
    $QuarantinePath = Resolve-FullPath $QuarantinePath
    Assert-SafeSiblingPaths -Canonical $DataRoot -Sibling $QuarantinePath
    $cycle = [ordered]@{
        schema = $script:CycleSchema
        cycle_id = $cycleId
        phase = "prepared"
        created_at_utc = Get-UtcTimestamp
        updated_at_utc = Get-UtcTimestamp
        source_commit = $sourceCommit
        runner_commit = $sourceCommit
        candidate = [ordered]@{
            source_commit = $sourceCommit
            path = $MsiPath
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $MsiPath).Hash
            size_bytes = (Get-Item -LiteralPath $MsiPath).Length
            signature_status = (Get-AuthenticodeSignature -LiteralPath $MsiPath).Status.ToString()
        }
        paths = [ordered]@{
            data_root = $DataRoot
            install_root = $InstallRoot
            quarantine = $QuarantinePath
            evidence_root = Split-Path -Parent $script:StatePath
        }
        retained = [ordered]@{ existed = $false; manifest_path = $null }
        install = [ordered]@{
            invocation_count = 0
            exit_code = $null
            installed_at_utc = $null
            adopted = $false
            log_path = Join-Path (Split-Path -Parent $script:StatePath) "msiexec.log"
        }
        smoke = [ordered]@{ runs = 0; last_run_at_utc = $null; evidence = $null }
        restore = [ordered]@{ restored_at_utc = $null; test_state_path = $null }
    }
    Save-Cycle $cycle
    Write-SafeSummary $cycle
    exit 0
}

if ($Action -eq "adopt") {
    if (-not $ConfirmAdopt) {
        throw "Adopting an existing install requires -ConfirmAdopt"
    }
    Assert-CleanTrackedTree
    if (Test-Path -LiteralPath $script:StatePath) {
        throw "Cycle state already exists; an installed cycle can only be adopted once"
    }
    if (-not $MsiPath -or -not $QuarantinePath -or -not $CandidateSourceCommit) {
        throw "Adopt requires -MsiPath, -CandidateSourceCommit, and -QuarantinePath"
    }
    if ($CandidateSourceCommit -notmatch "^[0-9a-fA-F]{40}$") {
        throw "CandidateSourceCommit must be a full Git commit SHA"
    }
    Invoke-Checked git @(
        "-C", $script:RepositoryRoot, "cat-file", "-e", "$CandidateSourceCommit^{commit}"
    )
    $MsiPath = Resolve-FullPath $MsiPath
    $QuarantinePath = Resolve-FullPath $QuarantinePath
    Assert-SafeSiblingPaths -Canonical $DataRoot -Sibling $QuarantinePath
    $service = Get-CimInstance Win32_Service -Filter "Name='UCIArenaManager'" -ErrorAction Stop
    if ($service.State -ne "Running" -or
        -not (Test-Path -LiteralPath $QuarantinePath -PathType Container)) {
        throw "Adopt requires a running Manager service and an existing quarantine"
    }
    $cycleId = "$($CandidateSourceCommit.Substring(0, 12))-adopted-$(Get-Date -AsUTC -Format 'yyyyMMdd-HHmmss')"
    $manifestPath = Join-Path (Split-Path -Parent $script:StatePath) "retained-state.json"
    $manifest = New-RetainedManifest -Root $QuarantinePath -OutputPath $manifestPath
    $cycle = [ordered]@{
        schema = $script:CycleSchema
        cycle_id = $cycleId
        phase = "installed"
        created_at_utc = Get-UtcTimestamp
        updated_at_utc = Get-UtcTimestamp
        source_commit = $CandidateSourceCommit.ToLowerInvariant()
        runner_commit = $sourceCommit
        candidate = [ordered]@{
            source_commit = $CandidateSourceCommit.ToLowerInvariant()
            path = $MsiPath
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $MsiPath).Hash
            size_bytes = (Get-Item -LiteralPath $MsiPath).Length
            signature_status = (Get-AuthenticodeSignature -LiteralPath $MsiPath).Status.ToString()
        }
        paths = [ordered]@{
            data_root = $DataRoot
            install_root = $InstallRoot
            quarantine = $QuarantinePath
            evidence_root = Split-Path -Parent $script:StatePath
        }
        retained = [ordered]@{
            existed = $manifest.existed
            manifest_path = $manifestPath
        }
        install = [ordered]@{
            invocation_count = 1
            exit_code = 0
            installed_at_utc = Get-UtcTimestamp
            adopted = $true
            log_path = $null
        }
        smoke = [ordered]@{ runs = 0; last_run_at_utc = $null; evidence = $null }
        restore = [ordered]@{ restored_at_utc = $null; test_state_path = $null }
    }
    Save-Cycle $cycle
    Write-SafeSummary $cycle
    exit 0
}

$cycle = Read-Cycle
if ($cycle.runner_commit -ne $sourceCommit) {
    throw "Cycle runner commit $($cycle.runner_commit) does not match checkout $sourceCommit"
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $cycle.candidate.path).Hash -ne
    $cycle.candidate.sha256) {
    throw "MSI candidate digest changed after cycle preparation"
}

if ($Action -eq "status") {
    Write-SafeSummary $cycle
    exit 0
}

if ($Action -eq "install") {
    Assert-Administrator
    if ($cycle.phase -ne "prepared" -or [int]$cycle.install.invocation_count -ne 0) {
        throw "Install is single-shot; this cycle is already $($cycle.phase) with $($cycle.install.invocation_count) MSI invocation(s)"
    }
    Assert-SafeSiblingPaths -Canonical $cycle.paths.data_root -Sibling $cycle.paths.quarantine
    if (Test-Path -LiteralPath $cycle.paths.quarantine) {
        throw "Quarantine already exists: $($cycle.paths.quarantine)"
    }
    $service = Get-Service -Name "UCIArenaManager" -ErrorAction SilentlyContinue
    if ($null -ne $service -and $service.Status -ne "Stopped") {
        Stop-Service -Name "UCIArenaManager" -Force
        (Get-Service -Name "UCIArenaManager").WaitForStatus("Stopped", [TimeSpan]::FromSeconds(20))
    }
    $manifestPath = Join-Path $cycle.paths.evidence_root "retained-state.json"
    $manifest = New-RetainedManifest -Root $cycle.paths.data_root -OutputPath $manifestPath
    $cycle.retained.existed = $manifest.existed
    $cycle.retained.manifest_path = $manifestPath
    if ($manifest.existed) {
        Rename-Item -LiteralPath $cycle.paths.data_root -NewName (
            Split-Path -Leaf $cycle.paths.quarantine
        )
    }
    $cycle.phase = "installing"
    $cycle.install.invocation_count = 1
    $cycle.updated_at_utc = Get-UtcTimestamp
    Save-Cycle $cycle
    $arguments = @(
        "/i", "`"$($cycle.candidate.path)`"", "/qn", "/norestart",
        "/l*v", "`"$($cycle.install.log_path)`""
    )
    $installer = Start-Process -FilePath "msiexec.exe" -ArgumentList $arguments -Wait -PassThru
    $cycle.install.exit_code = $installer.ExitCode
    if ($installer.ExitCode -notin @(0, 1641, 3010)) {
        if (Test-Path -LiteralPath $cycle.paths.data_root) {
            $failed = "$($cycle.paths.data_root).failed-$($cycle.cycle_id)"
            Rename-Item -LiteralPath $cycle.paths.data_root -NewName (Split-Path -Leaf $failed)
        }
        if ($cycle.retained.existed -and
            (Test-Path -LiteralPath $cycle.paths.quarantine) -and
            -not (Test-Path -LiteralPath $cycle.paths.data_root)) {
            Rename-Item -LiteralPath $cycle.paths.quarantine -NewName (
                Split-Path -Leaf $cycle.paths.data_root
            )
        }
        $cycle.phase = "install_failed"
        $cycle.updated_at_utc = Get-UtcTimestamp
        Save-Cycle $cycle
        throw "MSI install failed with exit code $($installer.ExitCode); original state was restored when possible"
    }
    $cycle.phase = "installed"
    $cycle.install.installed_at_utc = Get-UtcTimestamp
    $cycle.updated_at_utc = Get-UtcTimestamp
    Save-Cycle $cycle
    Write-SafeSummary $cycle
    exit 0
}

if ($Action -eq "smoke") {
    if ($cycle.phase -notin @("installed", "verified")) {
        throw "Smoke requires an installed or verified cycle; current phase is $($cycle.phase)"
    }
    if ($RestartService) {
        Assert-Administrator
        Restart-Service -Name "UCIArenaManager" -Force
        (Get-Service -Name "UCIArenaManager").WaitForStatus(
            "Running", [TimeSpan]::FromSeconds(20)
        )
    }
    $service = Get-CimInstance Win32_Service -Filter "Name='UCIArenaManager'" -ErrorAction Stop
    $requiredFiles = @(
        (Join-Path $cycle.paths.install_root "bin\arena.exe"),
        (Join-Path $cycle.paths.install_root "bin\arena-service.exe"),
        (Join-Path $cycle.paths.install_root "bin\arena-setup.exe"),
        (Join-Path $cycle.paths.install_root "share\uci-arena\web\index.html")
    )
    $missing = @($requiredFiles | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
    if ($service.State -ne "Running" -or $service.StartMode -ne "Auto" -or
        $service.StartName -ne "NT AUTHORITY\LocalService" -or $missing.Count -ne 0) {
        throw "Installed service or required package tree is invalid"
    }
    $arena = Join-Path $cycle.paths.install_root "bin\arena.exe"
    $setup = Join-Path $cycle.paths.install_root "bin\arena-setup.exe"
    $status = Invoke-Checked $arena @("--output", "json", "status") -Capture |
        ConvertFrom-Json -AsHashtable
    $health = Invoke-Checked $arena @("--output", "json", "health") -Capture |
        ConvertFrom-Json -AsHashtable
    $doctor = Invoke-Checked $arena @("--output", "json", "doctor") -Capture |
        ConvertFrom-Json -AsHashtable
    $keyPath = Join-Path $cycle.paths.data_root "client.key"
    $clientKey = (Get-Content -Raw -LiteralPath $keyPath).Trim()
    $base = "http://127.0.0.1:47831/api/v1"
    $authorization = @{ Authorization = "Bearer $clientKey" }
    $apiCodes = [ordered]@{
        unauthenticated = Get-HttpStatusCode "$base/system/status"
        invalid_bearer = Get-HttpStatusCode "$base/system/status" @{
            Authorization = "Bearer invalid"
        }
    }
    foreach ($path in @(
        "system/status", "engines", "match-plans", "tournaments",
        "engine-candidates", "resources/engine", "service-profiles", "events/snapshot"
    )) {
        $apiCodes[$path.Replace("/", "_")] = Get-HttpStatusCode "$base/$path" $authorization
    }
    if ($apiCodes.unauthenticated -ne 401 -or $apiCodes.invalid_bearer -ne 401 -or
        @($apiCodes.Values | Where-Object { $_ -notin @(200, 401) }).Count -ne 0) {
        throw "API authentication or collection smoke failed"
    }
    $pairing = Invoke-RestMethod -Uri "$base/auth/pair-token" -Method Post `
        -Headers $authorization -ContentType "application/json" -Body "{}"
    $browserSession = [Microsoft.PowerShell.Commands.WebRequestSession]::new()
    $pairResult = Invoke-RestMethod -Uri "$base/auth/pair" -Method Post `
        -WebSession $browserSession -ContentType "application/json" `
        -Body (@{ token = $pairing.pairing_token } | ConvertTo-Json -Compress)
    $pairedStatus = Invoke-WebRequest -Uri "$base/system/status" `
        -WebSession $browserSession -UseBasicParsing -TimeoutSec 10
    $dashboard = Invoke-WebRequest -Uri "http://127.0.0.1:47831/" `
        -WebSession $browserSession -UseBasicParsing -TimeoutSec 10
    if (-not $pairResult.csrf_token -or $pairedStatus.StatusCode -ne 200 -or
        $dashboard.StatusCode -ne 200 -or $dashboard.Content -notmatch "<html|<!doctype") {
        throw "Browser pairing or dashboard smoke failed"
    }
    $credentialPathImported = $false
    if ($LichessTokenFile) {
        $importText = Invoke-Checked $setup @(
            "--data-root", $cycle.paths.data_root, "--json", "import",
            "--kind", "lichess_token_path", "--path", $LichessTokenFile
        ) -Capture
        $import = $importText | ConvertFrom-Json -AsHashtable
        if ($import.validation.validation -ne "path_reference_only") {
            throw "Lichess credential locator was not handled as a path-only reference"
        }
        $credentialPathImported = $true
    }
    $probePaths = @()
    foreach ($suppliedPath in $ProbeEngine) {
        if (Test-Path -LiteralPath $suppliedPath -PathType Leaf) {
            $probePaths += $suppliedPath
            continue
        }
        $commaSeparated = @(
            $suppliedPath.Split(",", [StringSplitOptions]::RemoveEmptyEntries) |
                ForEach-Object { $_.Trim() }
        )
        if ($commaSeparated.Count -le 1 -or
            @($commaSeparated | Where-Object {
                -not (Test-Path -LiteralPath $_ -PathType Leaf)
            }).Count -ne 0) {
            throw "Probe engine path is unavailable: $suppliedPath"
        }
        $probePaths += $commaSeparated
    }
    $probes = @()
    foreach ($enginePath in $probePaths) {
        $probe = Invoke-Checked $arena @(
            "--output", "json", "engine", "probe", $enginePath,
            "--working-directory", (Split-Path -Parent $enginePath)
        ) -Capture | ConvertFrom-Json -AsHashtable
        if (-not $probe.ok -or -not $probe.bestmove) {
            throw "UCI probe failed"
        }
        $probes += [ordered]@{
            engine_name = $probe.engine_name
            fingerprint = $probe.fingerprint
            bestmove = $probe.bestmove
            option_count = $probe.option_count
            elapsed_ms = $probe.elapsed_ms
        }
    }
    if ($RequirePlanningEvidence -and
        ([int]$status.counts.engine -lt 2 -or [int]$status.counts.match_plan -lt 1 -or
         [int]$status.counts.tournament -lt 1)) {
        throw "Planning evidence requires two registered engines, one match plan, and one tournament in the fresh test state"
    }
    $evidence = [ordered]@{
        recorded_at_utc = Get-UtcTimestamp
        service = [ordered]@{
            state = $service.State
            start_mode = $service.StartMode
            start_name = $service.StartName
        }
        status = [ordered]@{
            database = $status.database
            counts = $status.counts
            reconciliation_state = $status.installed_service_reconciliation.state
            reconciliation_errors = @($status.installed_service_reconciliation.errors).Count
        }
        health_database = $health.database
        doctor_ok = $doctor.ok
        api_status_codes = $apiCodes
        dashboard_status = $dashboard.StatusCode
        browser_pairing = $true
        service_restarted = [bool]$RestartService
        lichess_token_path_imported = $credentialPathImported
        uci_probes = $probes
    }
    $cycle.phase = "verified"
    $cycle.smoke.runs = [int]$cycle.smoke.runs + 1
    $cycle.smoke.last_run_at_utc = Get-UtcTimestamp
    $cycle.smoke.evidence = $evidence
    $cycle.updated_at_utc = Get-UtcTimestamp
    Save-Cycle $cycle
    $evidence | ConvertTo-Json -Depth 10
    exit 0
}

if ($Action -eq "restore") {
    if (-not $ConfirmRestore) {
        throw "Restore moves the active test state and requires -ConfirmRestore"
    }
    Assert-Administrator
    if ($cycle.phase -notin @("installed", "verified")) {
        throw "Restore requires an installed or verified cycle"
    }
    $service = Get-Service -Name "UCIArenaManager" -ErrorAction SilentlyContinue
    if ($null -ne $service -and $service.Status -ne "Stopped") {
        Stop-Service -Name "UCIArenaManager" -Force
        (Get-Service -Name "UCIArenaManager").WaitForStatus("Stopped", [TimeSpan]::FromSeconds(20))
    }
    $testStatePath = "$($cycle.paths.data_root).test-$($cycle.cycle_id)"
    Assert-SafeSiblingPaths -Canonical $cycle.paths.data_root -Sibling $testStatePath
    if (Test-Path -LiteralPath $testStatePath) {
        throw "Test-state evidence target already exists: $testStatePath"
    }
    if (Test-Path -LiteralPath $cycle.paths.data_root) {
        Rename-Item -LiteralPath $cycle.paths.data_root -NewName (Split-Path -Leaf $testStatePath)
    }
    if ($cycle.retained.existed) {
        if (-not (Test-Path -LiteralPath $cycle.paths.quarantine -PathType Container)) {
            throw "Preserved state is missing from quarantine"
        }
        Rename-Item -LiteralPath $cycle.paths.quarantine -NewName (
            Split-Path -Leaf $cycle.paths.data_root
        )
        Assert-RetainedManifest -Root $cycle.paths.data_root `
            -ManifestPath $cycle.retained.manifest_path
        if ($null -ne $service) {
            Start-Service -Name "UCIArenaManager"
            (Get-Service -Name "UCIArenaManager").WaitForStatus(
                "Running", [TimeSpan]::FromSeconds(20)
            )
        }
    }
    $cycle.phase = "restored"
    $cycle.restore.restored_at_utc = Get-UtcTimestamp
    $cycle.restore.test_state_path = $testStatePath
    $cycle.updated_at_utc = Get-UtcTimestamp
    Save-Cycle $cycle
    Write-SafeSummary $cycle
    exit 0
}

throw "Unhandled action: $Action"
