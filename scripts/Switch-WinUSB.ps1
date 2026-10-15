# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('Plan','Bind','Restore')][string]$Mode = 'Plan',
    [string]$InstanceId = ''
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Device-Config.ps1')
$InstanceId=Resolve-XoneAdapterInstance $InstanceId
$taskPackage=Get-XoneAdapterPackage $InstanceId
$taskBinder = Join-Path $taskRoot 'outputs\xone-bind.exe'
$taskStatePath = Join-Path $taskRoot 'work\binding-state.json'
$taskRegPath = "HKLM:\SYSTEM\CurrentControlSet\Enum\$InstanceId"
$taskParamsPath = Join-Path $taskRegPath 'Device Parameters'
$taskGuid = '{215E9D83-9720-4A4C-8CF2-E8F40E366C21}'
if (-not (Test-Path -LiteralPath $taskBinder)) { throw 'Build outputs\xone-bind.exe first.' }

function Invoke-TaskBinder([string]$Action, [string]$Inf, [string]$Section) {
    & $taskBinder $Action --instance $InstanceId --inf $Inf --section $Section
    if ($LASTEXITCODE -eq 3010) {
        [void][IO.Directory]::CreateDirectory((Join-Path $taskRoot 'work'))
        @{Required=$true;Reason='Adapter binding change'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskRoot 'work/setup-restart-required.json') -Encoding UTF8
        Write-Warning 'Windows requests a restart. Setup will report it without restarting automatically.'
    }
    elseif ($LASTEXITCODE -ne 0) { throw "Driver helper failed (exit $LASTEXITCODE)." }
}
function Read-TaskProperty([string]$Key) {
    (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $Key -ErrorAction Stop).Data
}
function Save-TaskRegistryProperty([string]$Path,[string]$Name) {
    if (-not (Test-Path -LiteralPath $Path)) { return @{ Present=$false; Value=@() } }
    $taskProperties = Get-ItemProperty -LiteralPath $Path
    $taskProperty = $taskProperties.PSObject.Properties[$Name]
    if ($null -eq $taskProperty) { return @{ Present=$false; Value=@() } }
    return @{ Present=$true; Value=@($taskProperty.Value) }
}
function Restore-TaskRegistryProperty([string]$Path,[string]$Name,$Saved) {
    if ($Saved.Present) {
        if (-not (Test-Path -LiteralPath $Path)) { New-Item -Path $Path -Force | Out-Null }
        New-ItemProperty -LiteralPath $Path -Name $Name -Value ([string[]]$Saved.Value) -PropertyType MultiString -Force | Out-Null
    } elseif (Test-Path -LiteralPath $Path) {
        if ((Get-ItemProperty -LiteralPath $Path).PSObject.Properties[$Name]) {
            Remove-ItemProperty -LiteralPath $Path -Name $Name
        }
    }
}
function Restore-TaskRegistry($Saved) {
    Restore-TaskRegistryProperty $taskRegPath 'UpperFilters' $Saved.UpperFilters
    Restore-TaskRegistryProperty $taskParamsPath 'DeviceInterfaceGUIDs' $Saved.InterfaceGuids
}
function Assert-TaskState($Saved) {
    if ($Saved.InstanceId -ne $InstanceId -or -not (Test-XoneStockBinding $Saved)) {
        throw 'Saved recovery state does not match the expected adapter and stock driver.'
    }
}

$taskDevice = Get-PnpDevice -InstanceId $InstanceId -PresentOnly -ErrorAction Stop
$taskService = Read-TaskProperty 'DEVPKEY_Device_Service'
$taskInf = Read-TaskProperty 'DEVPKEY_Device_DriverInfPath'
Write-Output "Adapter: $($taskDevice.FriendlyName); service=$taskService; INF=$taskInf"
$taskWindowsInf = Join-Path $env:windir 'INF'
$taskWinusbInf = Join-Path $taskWindowsInf 'winusb.inf'

if ($Mode -eq 'Plan') {
    Invoke-TaskBinder 'plan' $taskWinusbInf 'WINUSB'
    if ($taskService -eq $taskPackage.Service) {
        $taskSection = (Read-TaskProperty 'DEVPKEY_Device_DriverInfSection') -replace '(?i)\.NT.*$',''
        Invoke-TaskBinder 'plan' (Join-Path $taskWindowsInf $taskInf) $taskSection
    }
    Write-Output 'No device settings, registry values, or driver bindings changed.'
    return
}
$taskPrincipal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Bind/Restore require an administrator PowerShell session. Plan does not.'
}

if ($Mode -eq 'Restore') {
    if (-not (Test-Path -LiteralPath $taskStatePath)) { throw 'No saved binding-state.json; use Device Manager and the exported driver backup.' }
    $taskSaved = Get-Content -LiteralPath $taskStatePath -Raw | ConvertFrom-Json
    Assert-TaskState $taskSaved
    Invoke-TaskBinder 'plan' (Join-Path $taskWindowsInf $taskSaved.InfName) $taskSaved.Section
    Restore-TaskRegistry $taskSaved
    Invoke-TaskBinder 'install' (Join-Path $taskWindowsInf $taskSaved.InfName) $taskSaved.Section
    Write-Output 'Stock binding restored. Unplug/replug the adapter, then run xone-probe xinput.'
    return
}
if ($taskService -ne $taskPackage.Service) { throw 'Bind requires the known Microsoft stock binding for this adapter model.' }
$taskSection = (Read-TaskProperty 'DEVPKEY_Device_DriverInfSection') -replace '(?i)\.NT.*$',''
$taskSaved = [pscustomobject]@{
    InstanceId=$InstanceId; Service=$taskService; InfName=$taskInf; Section=$taskSection
    DriverVersion=(Read-TaskProperty 'DEVPKEY_Device_DriverVersion')
    UpperFilters=(Save-TaskRegistryProperty $taskRegPath 'UpperFilters')
    InterfaceGuids=(Save-TaskRegistryProperty $taskParamsPath 'DeviceInterfaceGUIDs')
}
Assert-TaskState $taskSaved
Invoke-TaskBinder 'plan' $taskWinusbInf 'WINUSB'
Invoke-TaskBinder 'plan' (Join-Path $taskWindowsInf $taskInf) $taskSection
New-Item -ItemType Directory -Force -Path (Join-Path $taskRoot 'work\driver-backup') | Out-Null
& (Get-XoneSystemTool 'pnputil.exe') /export-driver $taskInf (Join-Path $taskRoot 'work\driver-backup')
if ($LASTEXITCODE -ne 0) { throw 'Cannot export the stock driver; binding was not changed.' }
if (Test-Path -LiteralPath $taskStatePath) {
    $taskPrevious = Get-Content -LiteralPath $taskStatePath -Raw | ConvertFrom-Json
    Assert-TaskState $taskPrevious
    if ($taskDevice.Status -ne 'OK' -or $taskPrevious.InfName -ne $taskInf) {
        throw 'Previous recovery state exists but stock restoration is not verified.'
    }
    $taskArchive = Join-Path $taskRoot ('work\binding-state-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.json')
    Move-Item -LiteralPath $taskStatePath -Destination $taskArchive
}
$taskSaved | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $taskStatePath -Encoding UTF8
try {
    # Only this instance changes. Leave other device filters and other GUIDs intact.
    $taskFilters = @($taskSaved.UpperFilters.Value | Where-Object { $_ -ne 'xboxgip' })
    if ($taskFilters.Count) {
        New-ItemProperty -LiteralPath $taskRegPath -Name UpperFilters -Value ([string[]]$taskFilters) -PropertyType MultiString -Force | Out-Null
    } elseif ($taskSaved.UpperFilters.Present) {
        Remove-ItemProperty -LiteralPath $taskRegPath -Name UpperFilters
    }
    if (-not (Test-Path -LiteralPath $taskParamsPath)) { New-Item -Path $taskParamsPath -Force | Out-Null }
    $taskGuids = @($taskSaved.InterfaceGuids.Value) + @($taskGuid)
    New-ItemProperty -LiteralPath $taskParamsPath -Name DeviceInterfaceGUIDs -Value ([string[]]($taskGuids | Select-Object -Unique)) -PropertyType MultiString -Force | Out-Null
    Invoke-TaskBinder 'install' $taskWinusbInf 'WINUSB'
    if ((Read-TaskProperty 'DEVPKEY_Device_Service') -ne 'WinUSB') { throw 'Binding verification failed.' }
    Write-Output 'WinUSB binding selected. Unplug/replug the adapter before radio-test.'
    Write-Output 'Controllers/headsets will not function through the stock driver during this experiment.'
} catch {
    $taskOriginalError = $_
    Write-Warning 'Binding failed; attempting to restore the saved registry and stock driver.'
    try {
        Restore-TaskRegistry $taskSaved
        Invoke-TaskBinder 'install' (Join-Path $taskWindowsInf $taskSaved.InfName) $taskSaved.Section
    } catch { Write-Warning "Automatic restoration failed: $_. Use Device Manager and work\driver-backup." }
    throw $taskOriginalError
}
