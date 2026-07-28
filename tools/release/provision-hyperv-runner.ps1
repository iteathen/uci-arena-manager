[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet(
        "windows-11",
        "windows-server",
        "lichess"
    )]
    [string]$RunnerLabel,
    [Parameter(Mandatory = $true)]
    [string]$VMName,
    [Parameter(Mandatory = $true)]
    [string]$OpeningBookPath,
    [Parameter(Mandatory = $true)]
    [string]$SyzygyPath,
    [Parameter(Mandatory = $true)]
    [string]$WorkspacePath,
    [string]$Repository = "iteathen/uci-arena-manager",
    [string]$HostAddress = "172.20.160.1",
    [string]$CredentialPath,
    [switch]$EnableLichess,
    [string]$LichessTokenDirectory,
    [string]$LichessConfigDirectory,
    [string]$LichessProfileDirectory
)

#Requires -RunAsAdministrator

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$runnerVersion = "2.336.0"
$runnerArchiveSha256 = "d59123a43003e357b0805b5d0f611d0bd2f65ab67d51bd070dd4e7a0f685c162"
$runnerArchiveUrl = "https://github.com/actions/runner/releases/download/v$runnerVersion/actions-runner-win-x64-$runnerVersion.zip"
$shareAccountName = "arena-vm-runner"
$bootstrapServiceName = "ArenaRunnerBootstrap"
$shareNames = [ordered]@{
    books = "ArenaBooks"
    syzygy = "ArenaSyzygy"
    workspace = "ArenaWorkspace"
}

function Assert-Directory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Description directory is unavailable: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Grant-DirectoryAccess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Identity,
        [Parameter(Mandatory = $true)]
        [ValidateSet("ReadAndExecute", "Modify")]
        [string]$Rights
    )
    $acl = Get-Acl -LiteralPath $Path
    $rule = [Security.AccessControl.FileSystemAccessRule]::new(
        $Identity,
        [Security.AccessControl.FileSystemRights]::$Rights,
        [Security.AccessControl.InheritanceFlags]"ContainerInherit, ObjectInherit",
        [Security.AccessControl.PropagationFlags]::None,
        [Security.AccessControl.AccessControlType]::Allow
    )
    [void]$acl.SetAccessRule($rule)
    Set-Acl -LiteralPath $Path -AclObject $acl
}

function Ensure-SmbShare {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Identity,
        [Parameter(Mandatory = $true)]
        [ValidateSet("Read", "Change")]
        [string]$Access
    )
    $existing = Get-SmbShare -Name $Name -ErrorAction SilentlyContinue
    if ($existing -and
        [IO.Path]::GetFullPath([string]$existing.Path) -ne [IO.Path]::GetFullPath($Path)) {
        throw "SMB share $Name already targets a different path"
    }
    if (-not $existing) {
        if ($Access -eq "Change") {
            New-SmbShare -Name $Name -Path $Path -ChangeAccess $Identity | Out-Null
        } else {
            New-SmbShare -Name $Name -Path $Path -ReadAccess $Identity | Out-Null
        }
        return
    }
    Grant-SmbShareAccess -Name $Name -AccountName $Identity `
        -AccessRight $Access -Force | Out-Null
}

function Get-OrCreateShareCredential {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $credential = Import-Clixml -LiteralPath $Path
        if ($credential.UserName -ne $shareAccountName) {
            throw "Stored VM share credential has an unexpected account name"
        }
        return $credential
    }

    $randomBytes = [byte[]]::new(24)
    $random = [Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $random.GetBytes($randomBytes)
    } finally {
        $random.Dispose()
    }
    $password = [Convert]::ToBase64String($randomBytes)
    $securePassword = ConvertTo-SecureString $password -AsPlainText -Force
    $credential = [Management.Automation.PSCredential]::new(
        $shareAccountName,
        $securePassword
    )
    $credential | Export-Clixml -LiteralPath $Path
    $acl = Get-Acl -LiteralPath $Path
    $acl.SetAccessRuleProtection($true, $false)
    $currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $rule = [Security.AccessControl.FileSystemAccessRule]::new(
        $currentIdentity,
        [Security.AccessControl.FileSystemRights]::FullControl,
        [Security.AccessControl.AccessControlType]::Allow
    )
    [void]$acl.SetAccessRule($rule)
    Set-Acl -LiteralPath $Path -AclObject $acl
    return $credential
}

function Ensure-LocalShareAccount {
    param([Parameter(Mandatory = $true)]$Credential)
    $account = Get-LocalUser -Name $shareAccountName -ErrorAction SilentlyContinue
    if ($account) {
        Set-LocalUser -Name $shareAccountName `
            -Password $Credential.Password `
            -PasswordNeverExpires $true
    } else {
        New-LocalUser -Name $shareAccountName `
            -Password $Credential.Password `
            -PasswordNeverExpires `
            -UserMayNotChangePassword `
            -AccountNeverExpires | Out-Null
    }
}

function Get-RunnerRegistrationToken {
    $token = & gh api --method POST `
        "repos/$Repository/actions/runners/registration-token" `
        --jq .token
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($token)) {
        throw "GitHub did not issue a runner registration token"
    }
    return $token.Trim()
}

function Get-VerifiedRunnerArchive {
    param([Parameter(Mandatory = $true)][string]$CacheDirectory)
    New-Item -ItemType Directory -Force -Path $CacheDirectory | Out-Null
    $archive = Join-Path $CacheDirectory "actions-runner-win-x64-$runnerVersion.zip"
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        Invoke-WebRequest -Uri $runnerArchiveUrl -OutFile $archive
    }
    $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $runnerArchiveSha256) {
        throw "GitHub runner archive digest mismatch"
    }
    return $archive
}

function Get-BootstrapServiceExecutable {
    param([Parameter(Mandatory = $true)][string]$CacheDirectory)
    $executable = Join-Path $CacheDirectory "arena-runner-bootstrap.exe"
    if (Test-Path -LiteralPath $executable -PathType Leaf) {
        return $executable
    }
    $source = @'
using System;
using System.Diagnostics;
using System.ServiceProcess;
using System.Threading;

public sealed class ArenaRunnerBootstrapService : ServiceBase
{
    private Process child;

    public ArenaRunnerBootstrapService()
    {
        ServiceName = "ArenaRunnerBootstrap";
        CanStop = true;
        AutoLog = true;
    }

    protected override void OnStart(string[] args)
    {
        ThreadPool.QueueUserWorkItem(_ =>
        {
            try
            {
                var start = new ProcessStartInfo
                {
                    FileName = Environment.ExpandEnvironmentVariables(
                        @"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"),
                    Arguments =
                        "-NoProfile -ExecutionPolicy Bypass " +
                        "-File C:\\arena-runner\\bootstrap.ps1",
                    WorkingDirectory = @"C:\arena-runner",
                    UseShellExecute = false,
                    CreateNoWindow = true
                };
                child = Process.Start(start);
                child.WaitForExit();
                ExitCode = child.ExitCode;
            }
            catch
            {
                ExitCode = 1;
            }
            finally
            {
                Stop();
            }
        });
    }

    protected override void OnStop()
    {
        if (child != null && !child.HasExited)
        {
            child.Kill();
            child.WaitForExit();
        }
    }

    public static void Main()
    {
        ServiceBase.Run(new ArenaRunnerBootstrapService());
    }
}
'@
    Add-Type -TypeDefinition $source `
        -ReferencedAssemblies "System.ServiceProcess.dll" `
        -OutputAssembly $executable `
        -OutputType WindowsApplication
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Failed to compile the Windows runner bootstrap service"
    }
    return $executable
}

function Stop-VMGracefully {
    param([Parameter(Mandatory = $true)]$VM)
    if ($VM.State -eq "Off") {
        return
    }
    if ($VM.State -ne "Running") {
        throw "VM $($VM.Name) must be Running or Off, not $($VM.State)"
    }
    Stop-VM -VM $VM -Confirm:$false
    $deadline = [DateTime]::UtcNow.AddMinutes(2)
    do {
        Start-Sleep -Seconds 2
        $VM = Get-VM -Name $VM.Name
    } while ($VM.State -ne "Off" -and [DateTime]::UtcNow -lt $deadline)
    if ($VM.State -ne "Off") {
        throw "VM $($VM.Name) did not shut down gracefully; it was not forced off"
    }
}

function Get-WindowsVolume {
    param([Parameter(Mandatory = $true)][uint32]$DiskNumber)
    $partitions = Get-Partition -DiskNumber $DiskNumber |
        Where-Object Type -ne "Reserved"
    foreach ($partition in $partitions) {
        $accessPathAdded = $false
        if (-not $partition.DriveLetter) {
            $partition | Add-PartitionAccessPath -AssignDriveLetter
            $accessPathAdded = $true
            $partition = Get-Partition -DiskNumber $DiskNumber `
                -PartitionNumber $partition.PartitionNumber
        }
        if ($partition.DriveLetter) {
            $root = "$($partition.DriveLetter):\"
            if (Test-Path -LiteralPath (Join-Path $root "Windows\System32\Config\SYSTEM")) {
                return [ordered]@{
                    root = $root
                    partition = $partition
                    access_path_added = $accessPathAdded
                }
            }
        }
        if ($accessPathAdded) {
            $partition | Remove-PartitionAccessPath `
                -AccessPath "$($partition.DriveLetter):\"
        }
    }
    throw "Mounted VHD has no recognizable Windows system volume"
}

function Set-BootstrapService {
    param(
        [Parameter(Mandatory = $true)]
        [string]$WindowsRoot,
        [Parameter(Mandatory = $true)]
        [string]$ImagePath
    )
    $hivePath = Join-Path $WindowsRoot "Windows\System32\Config\SYSTEM"
    $hiveName = "ArenaOfflineSystem"
    & reg.exe load "HKLM\$hiveName" $hivePath | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to load the offline Windows SYSTEM registry hive"
    }
    try {
        $current = (Get-ItemProperty "Registry::HKEY_LOCAL_MACHINE\$hiveName\Select").Current
        $serviceKey = "Registry::HKEY_LOCAL_MACHINE\$hiveName\ControlSet$($current.ToString('000'))\Services\$bootstrapServiceName"
        New-Item -Path $serviceKey -Force | Out-Null
        Set-ItemProperty -Path $serviceKey -Name Type -Type DWord -Value 0x10
        Set-ItemProperty -Path $serviceKey -Name Start -Type DWord -Value 2
        Set-ItemProperty -Path $serviceKey -Name ErrorControl -Type DWord -Value 1
        Set-ItemProperty -Path $serviceKey -Name ObjectName `
            -Type String -Value "LocalSystem"
        Set-ItemProperty -Path $serviceKey -Name ImagePath `
            -Type ExpandString -Value $ImagePath
        Set-ItemProperty -Path $serviceKey -Name DisplayName `
            -Type String -Value "UCI Arena runner bootstrap"
    } finally {
        [GC]::Collect()
        [GC]::WaitForPendingFinalizers()
        & reg.exe unload "HKLM\$hiveName" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to unload the offline Windows SYSTEM registry hive"
        }
    }
}

$OpeningBookPath = Assert-Directory $OpeningBookPath "Opening-book"
$SyzygyPath = Assert-Directory $SyzygyPath "Syzygy"
New-Item -ItemType Directory -Force -Path $WorkspacePath | Out-Null
$WorkspacePath = (Resolve-Path -LiteralPath $WorkspacePath).Path
if ([string]::IsNullOrWhiteSpace($CredentialPath)) {
    $CredentialPath = Join-Path $WorkspacePath ".arena-vm-share.credential.xml"
}

$credential = Get-OrCreateShareCredential $CredentialPath
Ensure-LocalShareAccount $credential
$shareIdentity = "$env:COMPUTERNAME\$shareAccountName"
Grant-DirectoryAccess $OpeningBookPath $shareIdentity "ReadAndExecute"
Grant-DirectoryAccess $SyzygyPath $shareIdentity "ReadAndExecute"
Grant-DirectoryAccess $WorkspacePath $shareIdentity "Modify"
Ensure-SmbShare $shareNames.books $OpeningBookPath $shareIdentity "Read"
Ensure-SmbShare $shareNames.syzygy $SyzygyPath $shareIdentity "Read"
Ensure-SmbShare $shareNames.workspace $WorkspacePath $shareIdentity "Change"

$firewallName = "UCI Arena VM SMB"
if (-not (Get-NetFirewallRule -DisplayName $firewallName -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName $firewallName `
        -Direction Inbound `
        -Action Allow `
        -Protocol TCP `
        -LocalPort 445 `
        -RemoteAddress "172.20.160.0/20" `
        -InterfaceAlias "vEthernet (Default Switch)" | Out-Null
}

$environment = [ordered]@{
    ARENA_VM_ASSET_ROOT = "\\$HostAddress\$($shareNames.books)"
    ARENA_VM_OPENING_BOOK_PATH = "\\$HostAddress\$($shareNames.books)"
    ARENA_VM_SYZYGY_PATH = "\\$HostAddress\$($shareNames.syzygy)"
    ARENA_VM_WRITABLE_ROOT = "\\$HostAddress\$($shareNames.workspace)\$RunnerLabel"
}

if ($EnableLichess) {
    $required = [ordered]@{
        token = $LichessTokenDirectory
        config = $LichessConfigDirectory
        profiles = $LichessProfileDirectory
    }
    foreach ($entry in $required.GetEnumerator()) {
        if ([string]::IsNullOrWhiteSpace($entry.Value)) {
            throw "Lichess $($entry.Key) directory is required"
        }
    }
    $LichessTokenDirectory = Assert-Directory $LichessTokenDirectory "Lichess token"
    $LichessConfigDirectory = Assert-Directory $LichessConfigDirectory "Lichess config"
    $LichessProfileDirectory = Assert-Directory $LichessProfileDirectory "Lichess profile"
    $lichessShares = [ordered]@{
        secrets = "ArenaLichessSecrets"
        config = "ArenaLichessConfig"
        profiles = "ArenaLichessProfiles"
    }
    Grant-DirectoryAccess $LichessTokenDirectory $shareIdentity "ReadAndExecute"
    Grant-DirectoryAccess $LichessConfigDirectory $shareIdentity "ReadAndExecute"
    Grant-DirectoryAccess $LichessProfileDirectory $shareIdentity "ReadAndExecute"
    Ensure-SmbShare $lichessShares.secrets $LichessTokenDirectory $shareIdentity "Read"
    Ensure-SmbShare $lichessShares.config $LichessConfigDirectory $shareIdentity "Read"
    Ensure-SmbShare $lichessShares.profiles $LichessProfileDirectory $shareIdentity "Read"
    $environment.ARENA_LICHESS_TOKEN_FILE =
        "\\$HostAddress\$($lichessShares.secrets)\lichess_bot_token.txt"
    $environment.ARENA_LICHESS_CONFIG_PATH =
        "\\$HostAddress\$($lichessShares.config)\config.uci_arena.template.yml"
    $environment.ARENA_LICHESS_SINGLE_GAME_PROFILE_PATH =
        "\\$HostAddress\$($lichessShares.profiles)\uci_arena_single_game_profile_v1.json"
    $environment.ARENA_LICHESS_TIME_PROFILE_PATH =
        "\\$HostAddress\$($lichessShares.profiles)\uci_time_profile_v3.json"
    $environment.ARENA_LICHESS_CAPACITY_PROFILE_PATH =
        "\\$HostAddress\$($lichessShares.profiles)\uci_arena_lichess_capacity_profile_v1.json"
}

$vm = Get-VM -Name $VMName -ErrorAction Stop
if ($vm.Generation -ne 2) {
    throw "Protected runner VM must be Generation 2"
}
Stop-VMGracefully $vm
$bootDrive = Get-VMHardDiskDrive -VM $vm |
    Where-Object ControllerLocation -eq 0 |
    Select-Object -First 1
$vhdPath = $bootDrive.Path
if ([string]::IsNullOrWhiteSpace($vhdPath)) {
    throw "VM $VMName has no boot VHD"
}
$bootControllerType = $bootDrive.ControllerType
$bootControllerNumber = $bootDrive.ControllerNumber
$bootControllerLocation = $bootDrive.ControllerLocation

$cacheDirectory = Join-Path $WorkspacePath "runner-cache"
$runnerArchive = Get-VerifiedRunnerArchive $cacheDirectory
$bootstrapExecutable = Get-BootstrapServiceExecutable $cacheDirectory
$registrationToken = Get-RunnerRegistrationToken
$sharePassword = $credential.GetNetworkCredential().Password
$runnerName = "$VMName-$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$customLabels = "ephemeral,uci-suite,$RunnerLabel"
$environmentLines = @(
    $environment.GetEnumerator() |
        ForEach-Object { "$($_.Key)=$($_.Value)" }
) -join "`r`n"
$tokenBase64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($registrationToken))
$passwordBase64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($sharePassword))

$mounted = $null
$volumeInfo = $null
$diskDetached = $false
try {
    Remove-VMHardDiskDrive -VMHardDiskDrive $bootDrive
    $diskDetached = $true
    $mounted = Mount-VHD -Path $vhdPath -Passthru
    $volumeInfo = Get-WindowsVolume $mounted.DiskNumber
    $guestRunnerRoot = Join-Path $volumeInfo.root "arena-runner"
    if (Test-Path -LiteralPath (Join-Path $guestRunnerRoot ".runner")) {
        throw "Guest runner is already configured; use a fresh disposable VM clone"
    }
    New-Item -ItemType Directory -Force -Path $guestRunnerRoot | Out-Null
    Expand-Archive -LiteralPath $runnerArchive `
        -DestinationPath $guestRunnerRoot `
        -Force
    Copy-Item -LiteralPath $bootstrapExecutable `
        -Destination (Join-Path $guestRunnerRoot "arena-runner-bootstrap.exe") `
        -Force
    [IO.File]::WriteAllText(
        (Join-Path $guestRunnerRoot ".env"),
        $environmentLines + "`r`n"
    )

    $bootstrapTemplate = @'
$ErrorActionPreference = "Stop"
$runnerRoot = "C:\arena-runner"
$logPath = Join-Path $runnerRoot "bootstrap.log"
try {
    $password = [Text.Encoding]::UTF8.GetString(
        [Convert]::FromBase64String("__PASSWORD_BASE64__")
    )
    $token = [Text.Encoding]::UTF8.GetString(
        [Convert]::FromBase64String("__TOKEN_BASE64__")
    )
    $securePassword = ConvertTo-SecureString $password -AsPlainText -Force
    $account = Get-LocalUser -Name "__ACCOUNT__" -ErrorAction SilentlyContinue
    if ($account) {
        Set-LocalUser -Name "__ACCOUNT__" -Password $securePassword `
            -PasswordNeverExpires $true
    } else {
        New-LocalUser -Name "__ACCOUNT__" -Password $securePassword `
            -PasswordNeverExpires -UserMayNotChangePassword `
            -AccountNeverExpires | Out-Null
    }
    & icacls.exe $runnerRoot /grant "__ACCOUNT__:(OI)(CI)M" /T /C | Out-Null
    Push-Location $runnerRoot
    try {
        & .\config.cmd --unattended --ephemeral --disableupdate `
            --url "https://github.com/__REPOSITORY__" `
            --token $token `
            --name "__RUNNER_NAME__" `
            --labels "__LABELS__" `
            --work "_work" `
            --runasservice `
            --windowslogonaccount ".\__ACCOUNT__" `
            --windowslogonpassword $password *> $logPath
        if ($LASTEXITCODE -ne 0) {
            throw "Runner configuration failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
    Get-Service -Name "actions.runner.*" |
        Set-Service -StartupType Automatic
    Get-Service -Name "actions.runner.*" |
        Where-Object Status -ne "Running" |
        Start-Service
    & sc.exe delete "__BOOTSTRAP_SERVICE__" | Out-Null
    $password = $null
    $token = $null
    Remove-Item -LiteralPath $PSCommandPath -Force
} catch {
    [IO.File]::AppendAllText(
        $logPath,
        [Environment]::NewLine + $_.Exception.Message + [Environment]::NewLine
    )
    exit 1
}
'@
    $bootstrap = $bootstrapTemplate.
        Replace("__PASSWORD_BASE64__", $passwordBase64).
        Replace("__TOKEN_BASE64__", $tokenBase64).
        Replace("__ACCOUNT__", $shareAccountName).
        Replace("__REPOSITORY__", $Repository).
        Replace("__RUNNER_NAME__", $runnerName).
        Replace("__LABELS__", $customLabels).
        Replace("__BOOTSTRAP_SERVICE__", $bootstrapServiceName)
    $bootstrapPath = Join-Path $guestRunnerRoot "bootstrap.ps1"
    [IO.File]::WriteAllText($bootstrapPath, $bootstrap)
    $imagePath = "C:\arena-runner\arena-runner-bootstrap.exe"
    Set-BootstrapService $volumeInfo.root $imagePath
} finally {
    $registrationToken = $null
    $sharePassword = $null
    if ($volumeInfo -and $volumeInfo.access_path_added) {
        $volumeInfo.partition | Remove-PartitionAccessPath `
            -AccessPath "$($volumeInfo.partition.DriveLetter):\"
    }
    if ($mounted) {
        Dismount-VHD -Path $vhdPath
    }
    if ($diskDetached) {
        Add-VMHardDiskDrive -VM $vm `
            -ControllerType $bootControllerType `
            -ControllerNumber $bootControllerNumber `
            -ControllerLocation $bootControllerLocation `
            -Path $vhdPath
        Set-VMFirmware -VM $vm `
            -FirstBootDevice (Get-VMHardDiskDrive -VM $vm |
                Where-Object Path -eq $vhdPath)
    }
}

Start-VM -Name $VMName | Out-Null
$deadline = [DateTime]::UtcNow.AddMinutes(5)
do {
    Start-Sleep -Seconds 5
    $runners = @(
        & gh api "repos/$Repository/actions/runners" --jq ".runners[]"
    )
    $online = $false
    if ($LASTEXITCODE -eq 0 -and $runners.Count) {
        foreach ($row in $runners) {
            $runner = $row | ConvertFrom-Json
            if ($runner.name -eq $runnerName -and $runner.status -eq "online") {
                $online = $true
                break
            }
        }
    }
} while (-not $online -and [DateTime]::UtcNow -lt $deadline)

if (-not $online) {
    throw "Runner $runnerName did not become online within five minutes"
}

[ordered]@{
    schema = "arena_hyperv_runner_provision_result_v1"
    state = "ready"
    vm = $VMName
    runner = $runnerName
    labels = @("self-hosted", "Windows", "X64", "ephemeral", "uci-suite", $RunnerLabel)
    opening_book = $environment.ARENA_VM_OPENING_BOOK_PATH
    syzygy = $environment.ARENA_VM_SYZYGY_PATH
    workspace = $environment.ARENA_VM_WRITABLE_ROOT
    lichess_enabled = [bool]$EnableLichess
} | ConvertTo-Json -Depth 5
