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
    [string]$Config,
    [Parameter(Mandatory = $true)]
    [string]$SingleGameProfile,
    [Parameter(Mandatory = $true)]
    [string]$TimeProfile,
    [Parameter(Mandatory = $true)]
    [string]$CapacityProfile,
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
    $Config,
    $SingleGameProfile,
    $TimeProfile,
    $CapacityProfile
)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Protected acceptance input is unavailable: $path"
    }
}

New-Item -ItemType Directory -Force $RunRoot | Out-Null
$common = @(
    "--catalog", (Resolve-Path -LiteralPath $Catalog).Path,
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

& $Setup @common apply `
    --preset custom `
    --component lichess.adapter `
    --offline `
    --yes
if ($LASTEXITCODE -ne 0) {
    throw "Candidate Lichess installation failed"
}
& $Setup @common verify
if ($LASTEXITCODE -ne 0) {
    throw "Candidate Lichess installation verification failed"
}

$node = Get-Entrypoint "node_runtime.private" "node"
$acceptance = Get-Entrypoint "lichess.adapter" "live_acceptance"
$engineReceipt = Get-Receipt "uci_arena.engine"
$modelRoot = Join-Path (Join-Path $engineReceipt.active_root "models") "default"
if (-not (Test-Path -LiteralPath $modelRoot -PathType Container)) {
    throw "Candidate model root is unavailable: $modelRoot"
}

& $node $acceptance `
    --confirm I_ACCEPT_ONE_STAGING_BOT_GAME `
    --token-file (Resolve-Path -LiteralPath $TokenFile).Path `
    --expected-account $ExpectedAccount `
    --opponent $Opponent `
    --config (Resolve-Path -LiteralPath $Config).Path `
    --repo-root $engineReceipt.active_root `
    --profile (Resolve-Path -LiteralPath $SingleGameProfile).Path `
    --time-profile (Resolve-Path -LiteralPath $TimeProfile).Path `
    --capacity-profile (Resolve-Path -LiteralPath $CapacityProfile).Path `
    --model-root $modelRoot `
    --runtime-dir (Join-Path $RunRoot "lichess-live") `
    --attempts 3 `
    --minutes 20
if ($LASTEXITCODE -ne 0) {
    throw "Protected Lichess acceptance did not complete exactly one staging game"
}
