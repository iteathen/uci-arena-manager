[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Setup,
    [Parameter(Mandatory = $true)]
    [string]$RunRoot,
    [Parameter(Mandatory = $true)]
    [string]$AssetRoot,
    [Parameter(Mandatory = $true)]
    [string]$ResultPath,
    [string]$OpeningBookPath,
    [string]$SyzygyPath,
    [string]$UciEnginePath,
    [string]$BookForgePath,
    [string]$EvidenceServicePath,
    [string]$LichessTokenFile
)

$ErrorActionPreference = "Stop"

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

function Resolve-AssetDirectory {
    param(
        [string]$ExplicitPath,
        [Parameter(Mandatory = $true)]
        [string[]]$DefaultNames,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Container)) {
            throw "$Description directory is unavailable: $ExplicitPath"
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }
    foreach ($name in $DefaultNames) {
        $candidate = Join-Path $AssetRoot $name
        if (Test-Path -LiteralPath $candidate -PathType Container) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "$Description directory was not found beneath the mounted asset root"
}

function Get-MetadataSnapshot {
    param([Parameter(Mandatory = $true)][string]$Path)
    $item = Get-Item -LiteralPath $Path
    $files = if ($item.PSIsContainer) {
        @(Get-ChildItem -LiteralPath $item.FullName -File |
            Sort-Object -Property Name)
    } else {
        @($item)
    }
    $rows = @(
        $files | ForEach-Object {
            [ordered]@{
                name = $_.Name
                length = [uint64]$_.Length
                last_write_utc_ticks = $_.LastWriteTimeUtc.Ticks
            }
        }
    )
    $metadata = $rows | ConvertTo-Json -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($metadata)
    $digest = [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData($bytes)
    ).ToLowerInvariant()
    return [ordered]@{
        file_count = $files.Count
        total_bytes = [uint64](($files | Measure-Object -Property Length -Sum).Sum)
        metadata_sha256 = $digest
    }
}

function Assert-Unchanged {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Kind,
        [Parameter(Mandatory = $true)]
        $Before,
        [Parameter(Mandatory = $true)]
        $After
    )
    if ($Before.file_count -ne $After.file_count -or
        $Before.total_bytes -ne $After.total_bytes -or
        $Before.metadata_sha256 -ne $After.metadata_sha256) {
        throw "Mounted $Kind metadata changed during in-place import"
    }
}

function Assert-Discovered {
    param(
        [Parameter(Mandatory = $true)]
        $Inventory,
        [Parameter(Mandatory = $true)]
        [string]$Kind,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedPath
    )
    $normalized = [IO.Path]::GetFullPath($ExpectedPath)
    $candidate = @($Inventory.discovery.$Kind.candidates) |
        Where-Object {
            $_.source -eq "discovered" -and
            [IO.Path]::GetFullPath([string]$_.path) -eq $normalized
        } |
        Select-Object -First 1
    if ($null -eq $candidate) {
        throw "Bounded mounted-root discovery did not find $Kind at $normalized"
    }
}

function Import-InPlace {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Kind,
        [Parameter(Mandatory = $true)]
        [string]$Path
    )
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $result = Invoke-JsonCommand $resolvedSetup (
        $common + @("import", "--kind", $Kind, "--path", $resolved)
    )
    if ($result.mode -ne "in_place_reference" -or $result.state -ne "saved" -or
        [IO.Path]::GetFullPath([string]$result.path) -ne
            [IO.Path]::GetFullPath($resolved)) {
        throw "$Kind import did not retain the exact in-place path"
    }
    $script:importedKinds.Add($Kind)
    return $result
}

$importedKinds = [System.Collections.Generic.List[string]]::new()

try {
    $resolvedSetup = (Resolve-Path -LiteralPath $Setup).Path
    if (-not (Test-Path -LiteralPath $AssetRoot -PathType Container)) {
        throw "Mounted asset root is unavailable: $AssetRoot"
    }
    $AssetRoot = (Resolve-Path -LiteralPath $AssetRoot).Path
    $OpeningBookPath = Resolve-AssetDirectory `
        $OpeningBookPath `
        @("books", "opening-books", "opening_books") `
        "Opening-book"
    $SyzygyPath = Resolve-AssetDirectory `
        $SyzygyPath `
        @("syzygy", "tablebases") `
        "Syzygy"

    New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null
    $common = @(
        "--install-root", (Join-Path $RunRoot "install"),
        "--data-root", (Join-Path $RunRoot "data"),
        "--cache-root", (Join-Path $RunRoot "cache"),
        "--workspace-root", (Join-Path $RunRoot "workspace"),
        "--knowledge-root", (Join-Path $RunRoot "knowledge"),
        "--json"
    )

    $before = [ordered]@{
        opening_book = Get-MetadataSnapshot $OpeningBookPath
        syzygy = Get-MetadataSnapshot $SyzygyPath
    }
    $discovery = Invoke-JsonCommand $resolvedSetup (
        $common + @("inventory", "--scan-root", $AssetRoot)
    )
    Assert-Discovered $discovery "opening_book" $OpeningBookPath
    Assert-Discovered $discovery "syzygy" $SyzygyPath

    [void](Import-InPlace "opening_book" $OpeningBookPath)
    [void](Import-InPlace "syzygy" $SyzygyPath)
    if (-not [string]::IsNullOrWhiteSpace($UciEnginePath)) {
        [void](Import-InPlace "uci_engine" $UciEnginePath)
    }
    if (-not [string]::IsNullOrWhiteSpace($BookForgePath)) {
        [void](Import-InPlace "book_forge" $BookForgePath)
    }
    if (-not [string]::IsNullOrWhiteSpace($EvidenceServicePath)) {
        [void](Import-InPlace "evidence_service" $EvidenceServicePath)
    }
    if (-not [string]::IsNullOrWhiteSpace($LichessTokenFile)) {
        if (-not (Test-Path -LiteralPath $LichessTokenFile -PathType Leaf)) {
            throw "Lichess token-file reference is unavailable"
        }
        [void](Import-InPlace "lichess_token_path" $LichessTokenFile)
    }

    $inventory = Invoke-JsonCommand $resolvedSetup ($common + @("inventory"))
    foreach ($kind in $importedKinds) {
        $selection = $inventory.discovery.$kind.selection
        if ($selection.source -ne "saved_locator") {
            throw "$kind did not resolve through its saved in-place locator"
        }
    }
    $receiptRoot = Join-Path $RunRoot "data/receipts"
    if ((Test-Path -LiteralPath $receiptRoot -PathType Container) -and
        @(Get-ChildItem -LiteralPath $receiptRoot -File).Count -ne 0) {
        throw "In-place mounted imports unexpectedly created managed receipts"
    }

    $after = [ordered]@{
        opening_book = Get-MetadataSnapshot $OpeningBookPath
        syzygy = Get-MetadataSnapshot $SyzygyPath
    }
    Assert-Unchanged "opening_book" $before.opening_book $after.opening_book
    Assert-Unchanged "syzygy" $before.syzygy $after.syzygy

    $result = [ordered]@{
        schema = "arena_vm_mounted_assets_acceptance_v1"
        state = "passed"
        asset_root = $AssetRoot
        imported_kinds = @($importedKinds)
        metadata = $after
        copy_performed = $false
        managed_receipts_created = 0
    }
    New-Item -ItemType Directory -Force `
        -Path (Split-Path -Parent $ResultPath) |
        Out-Null
    [IO.File]::WriteAllText(
        $ResultPath,
        ($result | ConvertTo-Json -Depth 20) + [Environment]::NewLine
    )
    $result | ConvertTo-Json -Depth 20
} catch {
    $failure = [ordered]@{
        schema = "arena_vm_mounted_assets_acceptance_v1"
        state = "failed"
        imported_kinds = @($importedKinds)
        error = $_.Exception.Message
    }
    New-Item -ItemType Directory -Force `
        -Path (Split-Path -Parent $ResultPath) |
        Out-Null
    [IO.File]::WriteAllText(
        $ResultPath,
        ($failure | ConvertTo-Json -Depth 20) + [Environment]::NewLine
    )
    throw
}
