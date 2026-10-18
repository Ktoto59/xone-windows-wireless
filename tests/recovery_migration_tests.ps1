# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $taskRoot 'scripts/Setup-Helpers.ps1')
function Check($Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Reject([scriptblock]$Operation,[string]$Message){$failed=$false;try{& $Operation | Out-Null}catch{$failed=$true};Check $failed $Message}
$fixture=Join-Path $taskRoot ('work/recovery-test-'+[guid]::NewGuid().ToString('N'))
$oldRoot=Join-Path $fixture 'old';$newRoot=Join-Path $fixture 'new'
$adapterId='USB\VID_045E&PID_02FE\TEST'
$script:recoveryInf=Join-Path $fixture 'installed-oem.inf'
$script:removedTask=$false
function Get-XoneRecoveryInf($State){return $script:recoveryInf}
function Get-ScheduledTask {param($TaskName,$ErrorAction) return @{TaskName=$TaskName;Actions=@(@{Execute=(Join-Path $oldRoot 'outputs/xone-background.exe');Arguments=''})}}
function Stop-ScheduledTask {param($TaskName,$ErrorAction)}
function Unregister-ScheduledTask {param($TaskName,$Confirm) $script:removedTask=$true}
function Get-XoneMemoryHost($Root){return $null}
function Stop-XoneManagedBridge($Root){throw 'Existing bridge should not be stopped when recovery is invalid'}
function Get-XoneDeviceService($InstanceId){return 'WinUSB'}
function Get-PnpDevice {param($InstanceId,[switch]$PresentOnly,$ErrorAction) return @{InstanceId=$adapterId;Status='OK'}}
try{
    [void][IO.Directory]::CreateDirectory((Join-Path $oldRoot 'work/driver-backup'))
    [IO.File]::WriteAllText($script:recoveryInf,'fixture INF')
    foreach($name in @('mt7612us_RL.inf','mt7612US.cat','mt7612US.sys')){
        [IO.File]::WriteAllText((Join-Path $oldRoot ('work/driver-backup/'+$name)),$(if($name.EndsWith('.inf')){'fixture INF'}else{'fixture binary; never executed'}))
    }
    $saved=@{InstanceId=$adapterId;Service='mt7612US';InfName='oem1.inf';Section='MTWLAN.ndi';UpperFilters=@{Present=$true;Value=@('xboxgip')};InterfaceGuids=@{Present=$false;Value=@()};IdlePower=@{SelectiveSuspendEnabled=@{Present=$false;Value=$null}}}
    Write-XoneJson (Join-Path $oldRoot 'work/binding-state.json') $saved
    $config=@{Version=1;AdapterInstanceId=$adapterId;AutoDetect=$true;BridgeBackend='cpp';Devices=@()}
    Write-XoneJson (Join-Path $oldRoot 'config/devices.local.json') $config
    # A transfer failure must retain the task pointing to the original folder.
    [IO.File]::WriteAllText((Join-Path $oldRoot 'work/driver-backup/mt7612us_RL.inf'),'incorrect INF')
    Reject {Move-XonePreviousInstallation $newRoot} 'Invalid original driver backup migrated'
    Check (-not $script:removedTask -and -not [IO.File]::Exists((Join-Path $newRoot 'work/binding-state.json'))) 'Failed migration lost the old task or published invalid metadata'
    [IO.File]::WriteAllText((Join-Path $oldRoot 'work/driver-backup/mt7612us_RL.inf'),'fixture INF')
    Move-XonePreviousInstallation $newRoot
    Check $script:removedTask 'Successful migration did not release old startup task'
    $copied=Assert-XoneRecovery $newRoot $adapterId
    Check ($copied.UpperFilters.Value[0] -eq 'xboxgip' -and $copied.IdlePower) 'Registry/power snapshots were lost'
    Check ((Read-XoneDeviceConfig (Join-Path $newRoot 'config/devices.local.json') -AllowEmpty).AdapterInstanceId -eq $adapterId) 'Adapter configuration was lost'
    foreach($file in Get-ChildItem -LiteralPath (Join-Path $oldRoot 'work/driver-backup') -File){
        Check ((Get-FileHash -LiteralPath $file.FullName).Hash -eq (Get-FileHash -LiteralPath (Join-Path $newRoot ('work/driver-backup/'+$file.Name))).Hash) 'Backup file changed during transfer'
    }
    Copy-XoneRecovery $oldRoot $newRoot $adapterId
    $partial=Join-Path $fixture 'partial'
    [void][IO.Directory]::CreateDirectory((Join-Path $partial 'work/driver-backup'))
    Copy-Item -LiteralPath (Join-Path $oldRoot 'work/driver-backup/mt7612US.sys') -Destination (Join-Path $partial 'work/driver-backup')
    Copy-XoneRecovery $oldRoot $partial $adapterId
    [void](Assert-XoneRecovery $partial $adapterId)
    Reject {Assert-XoneRecovery $newRoot 'USB\VID_045E&PID_02FE\OTHER'} 'Wrong adapter accepted'
    # Unknown WinUSB ownership must fail before declaring setup complete.
    $unowned=Join-Path $fixture 'unowned'
    Reject {Initialize-XoneApplication $unowned $adapterId} 'Unowned WinUSB was silently adopted'
    Check ($script:XoneSetupStage -eq 'RecoveryValidation' -and -not [IO.File]::Exists((Join-Path $unowned 'work/application-setup.json'))) 'Missing recovery was not rejected at the early setup guard'
    [IO.File]::WriteAllText((Join-Path $newRoot 'work/driver-backup/mt7612us_RL.inf'),'damaged copy')
    Reject {Assert-XoneRecovery $newRoot $adapterId} 'Damaged installed backup accepted'
    Write-Output 'PASS full recovery migration, hash checks, registry/power snapshots, retry/idempotence, task preservation on failure and early refusal of unowned WinUSB'
}finally{
    $resolved=[IO.Path]::GetFullPath($fixture)
    $boundary=[IO.Path]::GetFullPath((Join-Path $taskRoot 'work'))+[IO.Path]::DirectorySeparatorChar
    if(-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup outside work refused'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
