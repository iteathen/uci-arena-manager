[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Setup,
    [Parameter(Mandatory = $true)]
    [string]$Catalog,
    [Parameter(Mandatory = $true)]
    [string]$RunRoot,
    [Parameter(Mandatory = $true)]
    [string]$ResultPath,
    [switch]$AllowUnsignedDevelopmentCatalog
)

$ErrorActionPreference = "Stop"
$stages = [System.Collections.Generic.List[string]]::new()
$managerProcess = $null

function Invoke-JsonCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )
    $output = @(& $FilePath @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath exited with $LASTEXITCODE`: $($output -join [Environment]::NewLine)"
    }
    $text = $output -join [Environment]::NewLine
    try {
        return $text | ConvertFrom-Json
    } catch {
        throw "$FilePath emitted invalid JSON: $text"
    }
}

function Get-Receipt {
    param([string]$ComponentId)
    $filename = Join-Path $RunRoot "data\receipts\$ComponentId.json"
    if (-not (Test-Path -LiteralPath $filename -PathType Leaf)) {
        throw "missing receipt for $ComponentId"
    }
    return Get-Content -Raw -LiteralPath $filename | ConvertFrom-Json
}

function Get-Entrypoint {
    param(
        [string]$ComponentId,
        [string]$Name
    )
    $receipt = Get-Receipt $ComponentId
    $manifestPath = Join-Path $receipt.active_root "arena-component.json"
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    $relative = $manifest.entrypoints.$Name
    if (-not $relative) {
        throw "missing $ComponentId entrypoint $Name"
    }
    $entrypoint = Join-Path $receipt.active_root $relative
    if (-not (Test-Path -LiteralPath $entrypoint -PathType Leaf)) {
        throw "missing installed entrypoint $entrypoint"
    }
    return [string]$entrypoint
}

function Invoke-UciAcceptance {
    param(
        [string]$Engine,
        [string]$KnowledgeRoot,
        [string]$TracePath
    )
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Engine
    $start.WorkingDirectory = $env:TEMP
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables["UCI_ARENA_KNOWLEDGE_ROOT"] = $KnowledgeRoot
    $start.EnvironmentVariables.Remove("UCI_ARENA_MODEL_ROOT")
    $process = [System.Diagnostics.Process]::Start($start)
    $process.StandardInput.AutoFlush = $true
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds(120)
    $readLine = {
        param([string]$Phase)
        $remaining = [int][Math]::Max(
            1,
            ($deadline - [DateTimeOffset]::UtcNow).TotalMilliseconds
        )
        $pending = $process.StandardOutput.ReadLineAsync()
        if (-not $pending.Wait($remaining)) {
            throw "UCI engine timed out during $Phase after 120 seconds; see $TracePath"
        }
        $value = $pending.Result
        if ($null -ne $value) {
            Add-Content -LiteralPath $TracePath -Value $value
        }
        return $value
    }
    try {
        $process.StandardInput.WriteLine("uci")
        $modelLine = ""
        $modelRoot = ""
        while ($true) {
            $line = & $readLine "uci"
            if ($null -eq $line) {
                throw "UCI engine exited before uciok: $($process.StandardError.ReadToEnd())"
            }
            if ($line.StartsWith("info string model loaded:backend:flashfork:")) {
                $modelLine = $line
            }
            $prefix = "option name ModelRoot type string default "
            if ($line.StartsWith($prefix)) {
                $modelRoot = $line.Substring($prefix.Length)
            }
            if ($line -eq "uciok") {
                break
            }
        }
        if (-not $modelLine -or $modelLine -notmatch "fingerprint:(\d+):provider:([^:]+):") {
            throw "installed UCI engine did not load a typed model identity"
        }
        $fingerprint = $Matches[1]
        $provider = $Matches[2]
        $expectedModelRoot = Join-Path (Split-Path -Parent (Split-Path -Parent $Engine)) "models\default"
        if ([IO.Path]::GetFullPath($modelRoot) -ne [IO.Path]::GetFullPath($expectedModelRoot)) {
            throw "installed ModelRoot mismatch: $modelRoot"
        }
        $process.StandardInput.WriteLine("isready")
        while (($line = & $readLine "isready") -ne "readyok") {
            if ($null -eq $line) {
                throw "UCI engine exited before readyok"
            }
        }
        $process.StandardInput.WriteLine("position startpos")
        $process.StandardInput.WriteLine("go depth 1")
        $bestmove = ""
        while ($true) {
            $line = & $readLine "bestmove"
            if ($null -eq $line) {
                throw "UCI engine exited before bestmove"
            }
            if ($line -match "^bestmove ([a-h][1-8][a-h][1-8][qrbn]?)$") {
                $bestmove = $Matches[1]
                break
            }
        }
        return @{
            model_root = $modelRoot
            model_fingerprint = $fingerprint
            provider = $provider
            bestmove = $bestmove
        }
    } finally {
        if (-not $process.HasExited) {
            $process.StandardInput.WriteLine("quit")
            $process.StandardInput.Close()
            if (-not $process.WaitForExit(10000)) {
                $process.Kill()
            }
        }
        $process.Dispose()
    }
}

try {
    $resolvedSetup = (Resolve-Path -LiteralPath $Setup).Path
    $resolvedCatalog = (Resolve-Path -LiteralPath $Catalog).Path
    New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null
    $common = @(
        "--catalog", $resolvedCatalog,
        "--install-root", (Join-Path $RunRoot "install"),
        "--data-root", (Join-Path $RunRoot "data"),
        "--cache-root", (Join-Path $RunRoot "cache"),
        "--workspace-root", (Join-Path $RunRoot "workspace"),
        "--knowledge-root", (Join-Path $RunRoot "knowledge"),
        "--json"
    )
    if ($AllowUnsignedDevelopmentCatalog) {
        $common += "--allow-unsigned-development-catalog"
    }

    $plan = Invoke-JsonCommand $resolvedSetup (
        $common + @("plan", "--preset", "complete_arena", "--offline")
    )
    if (@($plan.blockers).Count -ne 0) {
        throw "Complete plan is blocked: $($plan.blockers | ConvertTo-Json -Compress)"
    }
    $stages.Add("plan")
    $apply = Invoke-JsonCommand $resolvedSetup (
        $common + @("apply", "--preset", "complete_arena", "--offline", "--yes")
    )
    if ($apply.state -ne "committed" -or @($apply.components).Count -lt 7) {
        throw "Complete apply did not commit every component"
    }
    $stages.Add("apply")
    $verify = Invoke-JsonCommand $resolvedSetup ($common + @("verify"))
    if (-not $verify.ok) {
        throw "installed component verification failed"
    }
    $stages.Add("verify")

    $node = Get-Entrypoint "node_runtime.private" "node"
    $engine = Get-Entrypoint "uci_arena.engine" "uci"
    $uciReceipt = Get-Receipt "uci_arena.engine"
    $uci = Invoke-UciAcceptance `
        $engine `
        (Join-Path $RunRoot "knowledge") `
        "$ResultPath.uci.log"
    $stages.Add("uci")

    $bookCli = Get-Entrypoint "book_forge.service" "cli"
    $bookReceipt = Get-Receipt "book_forge.service"
    $bookWorkspace = Join-Path $RunRoot "workspace\book-forge"
    $bookConfig = Join-Path $bookWorkspace "manager-config.json"
    [void](Invoke-JsonCommand $node @(
        $bookCli, "init", "--workspace", $bookWorkspace, "--config", $bookConfig
    ))
    $bookConfigDocument = Get-Content -Raw -LiteralPath $bookConfig | ConvertFrom-Json
    $bookConfigDocument | Add-Member -Force -NotePropertyName resources -NotePropertyValue @{
        minimumFreeWorkspaceGb = 1
        pauseAvailableMemoryGb = 1
        priorityProcessNames = @()
    }
    [IO.File]::WriteAllText(
        $bookConfig,
        ($bookConfigDocument | ConvertTo-Json -Depth 20) + [Environment]::NewLine
    )
    [void](Invoke-JsonCommand $node @(
        $bookCli, "source", "add", "--fixture", "bootstrap_v1",
        "--workspace", $bookWorkspace, "--config", $bookConfig
    ))
    $bookCycle = Invoke-JsonCommand $node @(
        $bookCli, "daemon", "--once",
        "--workspace", $bookWorkspace, "--config", $bookConfig
    )
    if (@($bookCycle.report.ingested).Count -ne 1 -or -not $bookCycle.report.integration.snapshotId) {
        throw "Book Forge did not ingest and publish its integration snapshot"
    }
    $bookStatus = Invoke-JsonCommand $node @(
        $bookCli, "status", "--workspace", $bookWorkspace, "--config", $bookConfig
    )
    $bookChannel = @($bookStatus.status.channels) |
        Where-Object { $_.channel -eq "integration" } |
        Select-Object -First 1
    $bookValidation = Invoke-JsonCommand $node @(
        $bookCli, "validate", $bookChannel.directory,
        "--workspace", $bookWorkspace, "--config", $bookConfig
    )
    if (-not $bookValidation.ok -or $bookValidation.id -ne $bookChannel.snapshot_id) {
        throw "Book Forge snapshot validation failed"
    }
    [void](Invoke-JsonCommand $node @(
        $bookCli, "startup", "install",
        "--workspace", $bookWorkspace, "--config", $bookConfig
    ))
    $bookStartup = Invoke-JsonCommand $node @(
        $bookCli, "startup", "status",
        "--workspace", $bookWorkspace, "--config", $bookConfig
    )
    if (-not $bookStartup.startup.installed) {
        throw "Book Forge startup registration is not installed"
    }
    [void](Invoke-JsonCommand $node @(
        $bookCli, "startup", "uninstall",
        "--workspace", $bookWorkspace, "--config", $bookConfig
    ))
    $stages.Add("book_forge")

    $evidenceCli = Get-Entrypoint "uci_arena_evidence.service" "cli"
    $evidenceWorkspace = Join-Path $RunRoot "workspace\evidence-service"
    $evidenceConfig = Join-Path $evidenceWorkspace "manager-config.json"
    [void](Invoke-JsonCommand $node @(
        $evidenceCli, "init", "--workspace", $evidenceWorkspace, "--config", $evidenceConfig
    ))
    $configDocument = Get-Content -Raw -LiteralPath $evidenceConfig | ConvertFrom-Json
    $configDocument | Add-Member -Force -NotePropertyName resources -NotePropertyValue @{
        consecutiveIdleSamples = 1
        inputIdleMs = 0
        maximumUsedMemoryPercent = 99
        maximumGpuUtilPercent = 100
        maximumUsedCpuPercent = 99
        minimumFreeWorkspaceGb = 1
        pauseAvailableMemoryGb = 1
        priorityProcessNames = @()
    }
    [IO.File]::WriteAllText(
        $evidenceConfig,
        ($configDocument | ConvertTo-Json -Depth 20) + [Environment]::NewLine
    )
    $target = Invoke-JsonCommand $node @(
        $evidenceCli, "target", "add",
        "--model-id", "flashfork_pv12_mobile_nnue_v1",
        "--model-root", $uci.model_root,
        "--fingerprint", $uci.model_fingerprint,
        "--engine", $engine,
        "--provider", $uci.provider,
        "--workspace", $evidenceWorkspace,
        "--config", $evidenceConfig
    )
    $jobConfig = @{
        campaign = "windows_clean_e2e"
        evidenceMode = "curve"
        movetimeMs = 1
        maxPairs = 1
        maxPlies = 4
        adjudicationCp = 0
        adjudicationDepth = 1
        candidateOptions = @("TimePolicy=RelativeClockV3")
        controlOptions = @()
    } | ConvertTo-Json -Compress
    $job = Invoke-JsonCommand $node @(
        $evidenceCli, "job", "enqueue",
        "--adapter", "paired_sprt",
        "--shard", "windows-clean-e2e",
        "--target", $target.id,
        "--config-json", $jobConfig,
        "--workspace", $evidenceWorkspace,
        "--config", $evidenceConfig
    )
    $evidenceRun = Invoke-JsonCommand $node @(
        $evidenceCli, "daemon", "--once",
        "--workspace", $evidenceWorkspace,
        "--config", $evidenceConfig
    )
    if ($evidenceRun.state -ne "completed" -or $evidenceRun.observations -lt 1) {
        throw "Evidence Service did not complete its minimal packaged job"
    }
    $datasets = @(Invoke-JsonCommand $node @(
        $evidenceCli, "dataset", "list",
        "--workspace", $evidenceWorkspace,
        "--config", $evidenceConfig
    ))
    $dataset = $datasets | Where-Object { $_.id -eq $job.datasetId } | Select-Object -First 1
    if ($dataset.state -ne "completed" -or $dataset.observationCount -lt 1) {
        throw "Evidence Service did not persist its dataset"
    }
    [void](Invoke-JsonCommand $node @(
        $evidenceCli, "startup", "install",
        "--workspace", $evidenceWorkspace, "--config", $evidenceConfig
    ))
    $evidenceStartup = Invoke-JsonCommand $node @(
        $evidenceCli, "startup", "status",
        "--workspace", $evidenceWorkspace, "--config", $evidenceConfig
    )
    if (-not $evidenceStartup.installed) {
        throw "Evidence Service startup registration is not installed"
    }
    [void](Invoke-JsonCommand $node @(
        $evidenceCli, "startup", "uninstall",
        "--workspace", $evidenceWorkspace, "--config", $evidenceConfig
    ))
    $stages.Add("evidence")

    $managerService = Get-Entrypoint "manager.core" "service"
    $managerCli = Get-Entrypoint "manager.core" "cli"
    $managerReceipt = Get-Receipt "manager.core"
    $webRoot = Join-Path $managerReceipt.active_root "share\uci-arena\web"
    $port = 18181
    $serviceStdout = Join-Path $RunRoot "manager-service.stdout.log"
    $serviceStderr = Join-Path $RunRoot "manager-service.stderr.log"
    $managerProcess = Start-Process -FilePath $managerService -ArgumentList @(
        "--data-dir", (Join-Path $RunRoot "data"),
        "--web-root", $webRoot,
        "--port", [string]$port
    ) -PassThru -WindowStyle Hidden -RedirectStandardOutput $serviceStdout `
        -RedirectStandardError $serviceStderr
    $endpoint = "http://127.0.0.1:$port"
    $ready = $false
    for ($attempt = 0; $attempt -lt 60; ++$attempt) {
        Start-Sleep -Milliseconds 250
        try {
            $health = Invoke-JsonCommand $managerCli @(
                "--endpoint", $endpoint,
                "--data-dir", (Join-Path $RunRoot "data"),
                "--output", "json",
                "health"
            )
            $ready = $true
            break
        } catch {}
    }
    if (-not $ready) {
        throw "Manager service did not become healthy"
    }
    $services = @(Invoke-JsonCommand $managerCli @(
        "--endpoint", $endpoint,
        "--data-dir", (Join-Path $RunRoot "data"),
        "--output", "json",
        "service", "list"
    ))
    $bookService = $services |
        Where-Object { $_.data.profile_id -eq "book_forge" } |
        Select-Object -First 1
    $evidenceService = $services |
        Where-Object { $_.data.profile_id -eq "uci_arena_evidence" } |
        Select-Object -First 1
    if (-not $bookService -or -not $evidenceService) {
        throw "Manager did not reconcile both installed optional services"
    }
    $bookProjection = Invoke-JsonCommand $managerCli @(
        "--endpoint", $endpoint,
        "--data-dir", (Join-Path $RunRoot "data"),
        "--output", "json",
        "service", "control", $bookService.id, "status"
    )
    $evidenceProjection = Invoke-JsonCommand $managerCli @(
        "--endpoint", $endpoint,
        "--data-dir", (Join-Path $RunRoot "data"),
        "--output", "json",
        "service", "control", $evidenceService.id, "status"
    )
    $bookData = $bookProjection.resource.data
    $evidenceData = $evidenceProjection.resource.data
    if ($bookData.integration_state -ne "available" -or
        $bookData.output_integration.authority -ne "book_forge" -or
        @($bookData.output_integration.snapshots)[0].snapshot_id -ne $bookValidation.id) {
        throw "Manager did not validate and project the Book Forge snapshot"
    }
    if ($evidenceData.integration_state -ne "available" -or
        $evidenceData.output_integration.authority -ne "uci_arena_evidence_service" -or
        @($evidenceData.output_integration.datasets)[0].id -ne $dataset.id) {
        throw "Manager did not validate and project the Evidence dataset"
    }
    $stages.Add("manager_integration")

    $result = @{
        schema = "arena_windows_complete_e2e_result_v1"
        state = "passed"
        stages = @($stages)
        components = @($apply.components).Count
        uci = $uci
        book_snapshot_id = $bookValidation.id
        evidence_dataset_id = $dataset.id
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ResultPath) | Out-Null
    [IO.File]::WriteAllText(
        $ResultPath,
        ($result | ConvertTo-Json -Depth 20) + [Environment]::NewLine
    )
    $result | ConvertTo-Json -Depth 20
} catch {
    $failure = @{
        schema = "arena_windows_complete_e2e_result_v1"
        state = "failed"
        stages = @($stages)
        error = $_.Exception.Message
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ResultPath) | Out-Null
    [IO.File]::WriteAllText(
        $ResultPath,
        ($failure | ConvertTo-Json -Depth 20) + [Environment]::NewLine
    )
    throw
} finally {
    if ($null -ne $managerProcess -and -not $managerProcess.HasExited) {
        Stop-Process -Id $managerProcess.Id -Force
        $managerProcess.WaitForExit()
    }
}
