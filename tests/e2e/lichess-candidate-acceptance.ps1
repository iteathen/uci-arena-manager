[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Setup,
    [Parameter(Mandatory = $true)]
    [string]$Catalog,
    [Parameter(Mandatory = $true)]
    [string]$RunRoot,
    [Parameter(Mandatory = $true)]
    [string]$TokenFile,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedAccount,
    [Parameter(Mandatory = $true)]
    [string]$Opponent,
    [Parameter(Mandatory = $true)]
    [string]$SingleGameProfile,
    [Parameter(Mandatory = $true)]
    [string]$TimeProfile,
    [Parameter(Mandatory = $true)]
    [string]$CapacityProfile,
    [Parameter(Mandatory = $true)]
    [string]$OpeningBookPath,
    [Parameter(Mandatory = $true)]
    [string]$SyzygyPath,
    [string]$WorkspaceRoot,
    [switch]$AllowUnsignedDevelopmentCatalog
)

$ErrorActionPreference = "Stop"

function Get-Receipt {
    param([Parameter(Mandatory = $true)][string]$ComponentId)
    $path = Join-Path (Join-Path $RunRoot "data/receipts") "$ComponentId.json"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing installed receipt for $ComponentId"
    }
    return Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
}

function Get-Entrypoint {
    param(
        [Parameter(Mandatory = $true)][string]$ComponentId,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $receipt = Get-Receipt $ComponentId
    $manifest = Get-Content -Raw -LiteralPath `
        (Join-Path $receipt.active_root "arena-component.json") |
        ConvertFrom-Json
    $relative = $manifest.entrypoints.$Name
    if (-not $relative) {
        throw "Missing $Name entrypoint for $ComponentId"
    }
    $path = Join-Path $receipt.active_root $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Installed entrypoint is unavailable: $path"
    }
    return [string]$path
}

foreach ($path in @(
    $Setup,
    $Catalog,
    $TokenFile,
    $SingleGameProfile,
    $TimeProfile,
    $CapacityProfile
)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Protected acceptance input is unavailable: $path"
    }
}
foreach ($path in @($OpeningBookPath, $SyzygyPath)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Protected acceptance asset path is unavailable: $path"
    }
}

New-Item -ItemType Directory -Force $RunRoot | Out-Null
if ([string]::IsNullOrWhiteSpace($WorkspaceRoot)) {
    $WorkspaceRoot = Join-Path $RunRoot "workspace"
}
New-Item -ItemType Directory -Force $WorkspaceRoot | Out-Null
$resolvedWorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
$common = @(
    "--catalog", (Resolve-Path -LiteralPath $Catalog).Path,
    "--install-root", (Join-Path $RunRoot "install"),
    "--data-root", (Join-Path $RunRoot "data"),
    "--cache-root", (Join-Path $RunRoot "cache"),
    "--workspace-root", $resolvedWorkspaceRoot,
    "--knowledge-root", (Join-Path $RunRoot "knowledge"),
    "--json"
)
if ($AllowUnsignedDevelopmentCatalog) {
    $common += "--allow-unsigned-development-catalog"
}

$tokenReference = @(
    & $Setup @common import `
        --kind lichess_token_path `
        --path (Resolve-Path -LiteralPath $TokenFile).Path 2>&1
) -join [Environment]::NewLine
if ($LASTEXITCODE -ne 0) {
    throw "Existing Lichess token-file reference import failed: $tokenReference"
}
$tokenImport = $tokenReference | ConvertFrom-Json
if ($tokenImport.mode -ne "in_place_reference" -or
    $tokenImport.validation.validation -ne "path_reference_only") {
    throw "Manager did not retain the existing Lichess token as a path-only reference"
}
$inventoryText = @(& $Setup @common inventory 2>&1) -join [Environment]::NewLine
if ($LASTEXITCODE -ne 0) {
    throw "Lichess token-file inventory failed: $inventoryText"
}
$inventory = $inventoryText | ConvertFrom-Json
if ($inventory.discovery.lichess_token_path.selection.source -ne "saved_locator") {
    throw "Manager did not select the saved existing-account token-file reference"
}

foreach ($asset in @(
    @{ Kind = "timing_profile"; Path = $TimeProfile },
    @{ Kind = "opening_book"; Path = $OpeningBookPath },
    @{ Kind = "syzygy"; Path = $SyzygyPath }
)) {
    $importText = @(
        & $Setup @common import `
            --kind $asset.Kind `
            --path (Resolve-Path -LiteralPath $asset.Path).Path 2>&1
    ) -join [Environment]::NewLine
    if ($LASTEXITCODE -ne 0) {
        throw "Existing $($asset.Kind) import failed: $importText"
    }
    $import = $importText | ConvertFrom-Json
    if ($import.mode -ne "in_place_reference" -or
        $import.state -ne "saved") {
        throw "Manager did not retain $($asset.Kind) as an in-place reference"
    }
}

& $Setup @common apply `
    --preset custom `
    --component lichess.adapter `
    --component book_forge.service `
    --component uci_arena_evidence.service `
    --offline `
    --yes
if ($LASTEXITCODE -ne 0) {
    throw "Candidate Lichess installation failed"
}
& $Setup @common verify
if ($LASTEXITCODE -ne 0) {
    throw "Candidate Lichess installation verification failed"
}

$initialEngineReceipt = Get-Receipt "uci_arena.engine"
$initialEngineRoot = [string]$initialEngineReceipt.active_root
& $Setup @common uninstall `
    --component uci_arena.engine `
    --yes
if ($LASTEXITCODE -ne 0) {
    throw "Candidate UCI Arena uninstall failed"
}
if (Test-Path -LiteralPath $initialEngineRoot) {
    throw "Candidate UCI Arena immutable version root survived uninstall"
}
if (Test-Path -LiteralPath (Join-Path (Join-Path $RunRoot "data/receipts") "uci_arena.engine.json")) {
    throw "Candidate UCI Arena receipt survived uninstall"
}
foreach ($path in @(
    $TokenFile,
    $TimeProfile,
    $OpeningBookPath,
    $SyzygyPath
)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Normal uninstall removed an imported external asset: $path"
    }
}
& $Setup @common apply `
    --preset custom `
    --component lichess.adapter `
    --component book_forge.service `
    --component uci_arena_evidence.service `
    --offline `
    --yes
if ($LASTEXITCODE -ne 0) {
    throw "Candidate UCI Arena reinstall failed"
}
& $Setup @common verify
if ($LASTEXITCODE -ne 0) {
    throw "Candidate UCI Arena reinstall verification failed"
}

$node = Get-Entrypoint "node_runtime.private" "node"
$acceptance = Get-Entrypoint "lichess.adapter" "live_acceptance"
$uciSmoke = Get-Entrypoint "lichess.adapter" "uci_smoke"
$engine = Get-Entrypoint "uci_arena.engine" "uci"
$evidenceCli = Get-Entrypoint "uci_arena_evidence.service" "cli"
$adapterReceipt = Get-Receipt "lichess.adapter"
$engineReceipt = Get-Receipt "uci_arena.engine"
$evidenceReceipt = Get-Receipt "uci_arena_evidence.service"
$Config = [string]$adapterReceipt.configuration_path
if (-not (Test-Path -LiteralPath $Config -PathType Leaf)) {
    throw "Manager-generated Lichess configuration is unavailable: $Config"
}
$installedConfig = Get-Content -Raw -LiteralPath $Config | ConvertFrom-Json
$resolvedBookRoot = (Resolve-Path -LiteralPath $OpeningBookPath).Path
$resolvedSyzygy = (Resolve-Path -LiteralPath $SyzygyPath).Path
$resolvedTimeProfile = (Resolve-Path -LiteralPath $TimeProfile).Path
if ([IO.Path]::GetFullPath([string]$installedConfig.engine.uci_options.SyzygyPath) -ne
    [IO.Path]::GetFullPath($resolvedSyzygy)) {
    throw "Installer did not hand the saved Syzygy locator to the installed engine"
}
$installedBookFile = [string]$installedConfig.engine.uci_options.BookFile
if (-not (Test-Path -LiteralPath $installedBookFile -PathType Leaf) -or
    [IO.Path]::GetFullPath((Split-Path -Parent $installedBookFile)) -ne
        [IO.Path]::GetFullPath(
            $(if (Test-Path -LiteralPath $resolvedBookRoot -PathType Container) {
                $resolvedBookRoot
            } else {
                Split-Path -Parent $resolvedBookRoot
            })
        )) {
    throw "Installer did not hand the saved opening-book locator to the installed engine"
}
if ([IO.Path]::GetFullPath([string]$installedConfig.engine.time_profile) -ne
    [IO.Path]::GetFullPath($resolvedTimeProfile)) {
    throw "Installer did not hand the saved timing profile to the installed adapter"
}

$evidenceConfigPath = [string]$evidenceReceipt.configuration_path
if (-not (Test-Path -LiteralPath $evidenceConfigPath -PathType Leaf)) {
    throw "Manager-generated Evidence Service configuration is unavailable"
}
$evidenceProfilesText = @(
    & $node $evidenceCli `
        --workspace ([string]$evidenceReceipt.data_paths[0]) `
        --config $evidenceConfigPath `
        profile list --json 2>&1
) -join [Environment]::NewLine
if ($LASTEXITCODE -ne 0) {
    throw "Installed Evidence Service timing-profile projection failed: $evidenceProfilesText"
}
$evidenceProfiles = @($evidenceProfilesText | ConvertFrom-Json)
$projectedProfile = @(
    $evidenceProfiles |
        Where-Object {
            $_.qualified -eq $true -and
            [IO.Path]::GetFullPath([string]$_.path) -eq
                [IO.Path]::GetFullPath($resolvedTimeProfile)
        }
)
if ($projectedProfile.Count -ne 1) {
    throw "Installed Evidence Service did not project the imported timing profile"
}
$TimeProfile = [string]$projectedProfile[0].path

$modelRoot = Join-Path (Join-Path $engineReceipt.active_root "models") "default"
if (-not (Test-Path -LiteralPath $modelRoot -PathType Container)) {
    throw "Candidate model root is unavailable: $modelRoot"
}
$evidenceConfig =
    Get-Content -Raw -LiteralPath $evidenceConfigPath |
    ConvertFrom-Json
$activeModelPointerPath = [string]$evidenceConfig.activeModelPointer
if (-not (Test-Path -LiteralPath $activeModelPointerPath -PathType Leaf)) {
    throw "Installer did not generate the Evidence Service active-model pointer"
}
$activeModelPointer =
    Get-Content -Raw -LiteralPath $activeModelPointerPath |
    ConvertFrom-Json
$qualifiedProfile =
    Get-Content -Raw -LiteralPath (Resolve-Path -LiteralPath $TimeProfile) |
    ConvertFrom-Json
$installedEngineSha256 =
    (Get-FileHash -Algorithm SHA256 -LiteralPath $Engine).Hash.ToLowerInvariant()
$declaredEngineSha256 =
    ([string]$activeModelPointer.engine_sha256 -replace '^sha256:', '').ToLowerInvariant()
if ($activeModelPointer.model_id -ne $qualifiedProfile.identity.model.model_id -or
    $activeModelPointer.model_fingerprint -ne
        $qualifiedProfile.identity.model.model_fingerprint -or
    $activeModelPointer.provider -ne
        $qualifiedProfile.identity.search.provider_effective -or
    [IO.Path]::GetFullPath([string]$activeModelPointer.engine_path) -ne
        [IO.Path]::GetFullPath($Engine) -or
    [IO.Path]::GetFullPath([string]$activeModelPointer.model_root) -ne
        [IO.Path]::GetFullPath($modelRoot) -or
    $declaredEngineSha256 -ne $installedEngineSha256) {
    throw "Installer-generated Evidence active-model pointer does not match the imported timing identity"
}

$uciSmokeText = @(
    & $node $uciSmoke `
        --config $Config `
        --repo-root $engineReceipt.active_root `
        --depth 1 `
        --timeout 120 2>&1
) -join [Environment]::NewLine
if ($LASTEXITCODE -ne 0) {
    throw "Installed engine asset integration smoke failed: $uciSmokeText"
}
$uciSmokeResult = $uciSmokeText | ConvertFrom-Json
if ($uciSmokeResult.opening_book_active -ne $true -or
    [int64]$uciSmokeResult.opening_book_entries -lt 1) {
    throw "Installed engine did not load the Manager-selected opening book"
}
if ($uciSmokeResult.syzygy_active -ne $true -or
    [int]$uciSmokeResult.syzygy_loaded_cardinality -lt 3) {
    throw "Installed engine did not load the Manager-selected Syzygy tables"
}

$acceptanceArguments = @(
    "--confirm", "I_ACCEPT_ONE_BOT_GAME",
    "--token-file", (Resolve-Path -LiteralPath $TokenFile).Path,
    "--expected-account", $ExpectedAccount,
    "--opponent", $Opponent,
    "--config", (Resolve-Path -LiteralPath $Config).Path,
    "--repo-root", $engineReceipt.active_root,
    "--engine", $engine,
    "--working-directory", $engineReceipt.active_root,
    "--profile", (Resolve-Path -LiteralPath $SingleGameProfile).Path,
    "--capacity-profile", (Resolve-Path -LiteralPath $CapacityProfile).Path,
    "--model-root", $modelRoot,
    "--runtime-dir", (Join-Path $RunRoot "lichess-live"),
    "--attempts", "3",
    "--minutes", "20"
)
$acceptanceArguments += @(
    "--time-profile",
    (Resolve-Path -LiteralPath $TimeProfile).Path
)

$liveAcceptanceText = @(
    & $node $acceptance @acceptanceArguments 2>&1
) -join [Environment]::NewLine
if ($LASTEXITCODE -ne 0) {
    throw "Protected Lichess acceptance did not complete exactly one existing-account game: $liveAcceptanceText"
}
$liveAcceptance = $liveAcceptanceText | ConvertFrom-Json
if ($liveAcceptance.status -ne "passed") {
    throw "Protected Lichess acceptance did not report a passing game"
}
$runtimePath = Join-Path (Join-Path $RunRoot "lichess-live") "runtime.json"
if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
    throw "Protected Lichess runtime evidence is unavailable"
}
$runtime = Get-Content -Raw -LiteralPath $runtimePath | ConvertFrom-Json
if ($runtime.timing_profile.status -ne "accepted" -or
    $runtime.timing_profile.accepted -ne $true -or
    $runtime.timing_profile.applied -ne $true -or
    $runtime.timing_profile.permissions.provider_safety_qualified -ne $true -or
    [int64]$runtime.timing_profile.provider_service_reserve_us -le 0) {
    throw "Installed timing profile was not accepted and applied during the live game"
}
$liveAcceptance | ConvertTo-Json -Depth 20
