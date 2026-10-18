# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $taskRoot 'scripts/Setup-Helpers.ps1')
. (Join-Path $taskRoot 'scripts/Mode-Helpers.ps1')
function Check($Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Reject([scriptblock]$Operation,[string]$Message){$failed=$false;try{& $Operation | Out-Null}catch{$failed=$true};Check $failed $Message}
$fixture=Join-Path $taskRoot ('work/mode-test-'+[guid]::NewGuid().ToString('N')+' spaced path')
$global:modeMock=[pscustomobject]@{Service='WinUSB';Status='OK';Present=$true;Firmware=$true;Usbip=$true;RadioIdle=$true;SwitchFails=$false;Restart=0;Approval='Allow';Calls=[Collections.Generic.List[string]]::new();Task=$null;Boot=[datetime]::UtcNow.AddDays(-1)}
$adapterId='USB\VID_045E&PID_02FE\TEST'
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
function Get-PnpDevice {param($InstanceId,[switch]$PresentOnly,$ErrorAction)
    if(-not $modeMock.Present){if($ErrorAction -eq 'Stop'){throw 'Adapter is unplugged'};return $null}
    return [pscustomobject]@{InstanceId=$adapterId;Status=$modeMock.Status}
}
function Get-XoneDeviceService([string]$InstanceId){return $modeMock.Service}
function Test-XoneFirmware([string]$Root){return $modeMock.Firmware}
function Get-XoneUsbipStatus {return @{Ready=$modeMock.Usbip}}
function Stop-XoneManagedBridge([string]$Root){$modeMock.Calls.Add('Stop')}
function Assert-XoneRadioIdle {if(-not $modeMock.RadioIdle){throw 'Foreign radio is active'}}
function Invoke-XonePnputil([string[]]$Arguments){Check ($Arguments[0] -eq '/restart-device' -and $Arguments[1] -eq $adapterId) 'Restart targeted an unrelated device';$modeMock.Calls.Add('Restart');return $modeMock.Restart}
function Get-CimInstance {param($ClassName,$ErrorAction) Check ($ClassName -eq 'Win32_OperatingSystem') 'Unexpected CIM call';return @{LastBootUpTime=$modeMock.Boot}}
function Get-ScheduledTask {param($TaskName,$ErrorAction) return $modeMock.Task}
function Start-Process {param($FilePath,$ArgumentList,$WorkingDirectory,$Verb,$WindowStyle,[switch]$PassThru,[switch]$Wait)
    Check ($Verb -eq 'RunAs' -and $WindowStyle -eq 'Hidden' -and $PassThru -and $Wait) 'Driver operation did not use waited hidden elevation'
    Check ($FilePath -eq (Get-XoneSystemTool 'WindowsPowerShell/v1.0/powershell.exe') -and $WorkingDirectory -eq $fixture) 'Elevated operation lost native shell or installation root'
    Check ($ArgumentList.Contains('"'+(Join-Path $fixture 'scripts/Setup-Action.ps1')+'"') -and $ArgumentList.Contains('"'+$adapterId+'"')) 'Elevated arguments lost spaced paths or adapter identity'
    $modeMock.Calls.Add('UAC')
    if($modeMock.Approval -eq 'Cancel'){throw 'Simulated UAC cancellation'}
    $target=if($ArgumentList.Contains('-Action SwitchToBridge')){'Bridge'}else{'Stock'}
    $resultPath=Join-Path $fixture ('work/mode-'+$target+'.json')
    if($modeMock.Approval -eq 'Missing'){return @{ExitCode=0}}
    if($modeMock.Approval -eq 'Failure'){Write-XoneJson $resultPath @{Success=$false;Error='Simulated driver failure'};return @{ExitCode=1}}
    if($modeMock.Approval -eq 'WrongDriver'){Write-XoneJson $resultPath @{Success=$true;AdapterMode=@{Changed=$true;RestartRequired=$false;ReconnectRequired=$false}};return @{ExitCode=0}}
    $value=Set-XoneAdapterMode $fixture $target $adapterId
    Write-XoneJson $resultPath @{Success=$true;AdapterMode=$value}
    return @{ExitCode=0}
}
try{
    [void][IO.Directory]::CreateDirectory((Join-Path $fixture 'scripts'))
    Write-XoneJson (Join-Path $fixture 'config/app.local.json') @{Version=1;OwnerSid=$sid;AutoStart=$true}
    Write-XoneJson (Join-Path $fixture 'config/devices.local.json') @{Version=1;AdapterInstanceId=$adapterId;Devices=@();AutoDetect=$true;BridgeBackend='cpp'}
    $backup=@{InstanceId=$adapterId;Service='mt7612US';InfName='oem1.inf';Section='MTWLAN.ndi'}
    Write-XoneJson (Join-Path $fixture 'work/binding-state.json') $backup
    [IO.File]::WriteAllText((Join-Path $fixture 'scripts/Switch-WinUSB.ps1'),@'
param($Mode,$InstanceId)
if($InstanceId -ne 'USB\VID_045E&PID_02FE\TEST'){throw 'Wrong adapter selected'}
$global:modeMock.Calls.Add($Mode)
if($global:modeMock.SwitchFails){throw 'Simulated bind/restore failure'}
$global:modeMock.Service=if($Mode -eq 'Bind'){'WinUSB'}else{'mt7612US'}
'@)
    [IO.File]::WriteAllText((Join-Path $fixture 'scripts/Set-AutomaticBridge.ps1'),@'
param($Mode,[switch]$NoStart)
$global:modeMock.Calls.Add(('Task:'+ $Mode + ':' + [bool]$NoStart))
'@)
    $result=Invoke-XoneModeSwitch $fixture Stop
    Check ($result.Mode -eq 'Stock' -and $result.Changed -and $modeMock.Service -eq 'mt7612US') 'Stop failed to restore stock'
    Check (($modeMock.Calls -join ',') -eq 'Task:Disable:False,UAC,Stop,Restore,Restart') 'Stop did not pause task before restoring driver'
    Check ((Read-XoneJson (Join-Path $fixture 'config/app.local.json')).AutoStart) 'Stop destroyed saved startup preference'
    $modeMock.Calls.Clear();$result=Invoke-XoneModeSwitch $fixture Stop
    Check (-not $result.Changed -and ($modeMock.Calls -join ',') -eq 'Task:Disable:False') 'Repeated Stop rebound the adapter or asked for UAC'
    $modeMock.Calls.Clear();$result=Invoke-XoneModeSwitch $fixture Start
    Check ($result.Mode -eq 'Bridge' -and $result.Changed -and $modeMock.Service -eq 'WinUSB') 'Start failed to select WinUSB'
    Check (($modeMock.Calls -join ',') -eq 'UAC,Stop,Bind,Restart,Task:Enable:True,Task:Start:False') 'Start launched before driver verification or lost startup preference'
    $modeMock.Calls.Clear();$result=Invoke-XoneModeSwitch $fixture Start
    Check (-not $result.Changed -and -not $modeMock.Calls.Contains('UAC') -and -not $modeMock.Calls.Contains('Bind')) 'Repeated Start rebound the adapter or requested elevation'
    Write-XoneJson (Join-Path $fixture 'config/app.local.json') @{Version=1;OwnerSid=$sid;AutoStart=$false}
    $modeMock.Calls.Clear();$result=Invoke-XoneModeSwitch $fixture Start
    Check (($modeMock.Calls -join ',') -eq 'Task:Start:False') 'Manual Start enabled opted-out sign-in startup'
    Write-Output 'PASS stock/bridge round trip, idempotent shortcuts, ordered driver operations and preserved startup opt-out'

    foreach($approval in @('Cancel','Missing','Failure','WrongDriver')){
        $modeMock.Service='mt7612US';$modeMock.Approval=$approval;$modeMock.Calls.Clear()
        Write-XoneJson (Join-Path $fixture 'work/mode-Bridge.json') @{Success=$true;AdapterMode=@{Changed=$true}}
        Reject {Invoke-XoneModeSwitch $fixture Start} ('Approval failure accepted: '+$approval)
        Check (-not $modeMock.Calls.Contains('Task:Start:False')) 'Approval failure started the bridge'
        if($approval -in @('Cancel','Missing')){Check (-not [IO.File]::Exists((Join-Path $fixture 'work/mode-Bridge.json'))) 'Stale successful result survived failed approval'}
    }
    $modeMock.Service='WinUSB';$modeMock.Approval='Cancel';$modeMock.Calls.Clear()
    Reject {Invoke-XoneModeSwitch $fixture Stop} 'Cancelled Stop returned success'
    Check (($modeMock.Calls -join ',') -eq 'Task:Disable:False,UAC' -and $modeMock.Service -eq 'WinUSB') 'Cancelled Stop changed driver or failed to pause the bridge task'
    $modeMock.Service='mt7612US'
    $modeMock.Approval='Allow';$modeMock.SwitchFails=$true;$modeMock.Calls.Clear()
    Reject {Invoke-XoneModeSwitch $fixture Start} 'Driver failure returned success'
    Check (-not $modeMock.Calls.Contains('Task:Start:False')) 'Driver failure started the bridge'
    $modeMock.SwitchFails=$false
    $modeMock.RadioIdle=$false;$modeMock.Calls.Clear()
    Reject {Invoke-XoneModeSwitch $fixture Start} 'Foreign radio accepted'
    Check (-not $modeMock.Calls.Contains('Bind')) 'Foreign radio did not prevent rebinding'
    $modeMock.RadioIdle=$true
    $modeMock.Restart=5;$modeMock.Calls.Clear()
    Reject {Invoke-XoneModeSwitch $fixture Start} 'Failed device restart returned success'
    Check (-not $modeMock.Calls.Contains('Task:Start:False')) 'Reconnect-required operation started the bridge'
    $modeMock.Service='mt7612US';$modeMock.Restart=3010;$modeMock.Calls.Clear()
    Reject {Invoke-XoneModeSwitch $fixture Start} 'Restart-required operation returned success'
    Check (-not $modeMock.Calls.Contains('Task:Start:False')) 'Restart-required operation started the bridge'
    $modeMock.Restart=0;$modeMock.Calls.Clear()
    Reject {Invoke-XoneModeSwitch $fixture Start} 'Pending restart was bypassed on a repeated Start'
    Check ($modeMock.Calls.Count -eq 0) 'Pending restart started work'
    $modeMock.Boot=[datetime]::UtcNow.AddSeconds(1)
    Check (-not (Test-XoneModeRestartPending $fixture)) 'Completed reboot did not clear old marker'
    $modeMock.Boot=[datetime]::UtcNow.AddDays(-1)
    Write-Output 'PASS cancelled/failed UAC, missing/stale results, binding failures, foreign radio and restart/reconnect handling'

    $modeMock.Service='WinUSB';$modeMock.Calls.Clear()
    [IO.File]::Delete((Join-Path $fixture 'work/binding-state.json'))
    Reject {Invoke-XoneModeSwitch $fixture Stop} 'Unowned WinUSB accepted'
    Check (($modeMock.Calls -join ',') -eq 'Task:Disable:False' -and $modeMock.Service -eq 'WinUSB') 'Missing backup changed driver or failed to stop the owned runtime'
    $backup.InstanceId='USB\VID_045E&PID_02FE\OTHER';Write-XoneJson (Join-Path $fixture 'work/binding-state.json') $backup
    Reject {Invoke-XoneModeSwitch $fixture Stop} 'Backup for another adapter accepted'
    $backup.InstanceId=$adapterId;Write-XoneJson (Join-Path $fixture 'work/binding-state.json') $backup
    Reject {Set-XoneAdapterMode $fixture Stock 'USB\VID_045E&PID_02FE\OTHER'} 'Explicit foreign adapter accepted'
    $modeMock.Calls.Clear();$modeMock.Present=$false;Reject {Invoke-XoneModeSwitch $fixture Stop} 'Unplugged adapter accepted';$modeMock.Present=$true
    Check (($modeMock.Calls -join ',') -eq 'Task:Disable:False') 'Unplugged adapter prevented stopping the owned runtime'
    $modeMock.Status='Error';Reject {Invoke-XoneModeSwitch $fixture Start} 'Unready WinUSB accepted';$modeMock.Status='OK'
    $modeMock.Service='Unknown';Reject {Invoke-XoneModeSwitch $fixture Stop} 'Unknown driver accepted';$modeMock.Service='WinUSB'
    $modeMock.Firmware=$false;Reject {Invoke-XoneModeSwitch $fixture Start} 'Missing firmware accepted';$modeMock.Firmware=$true
    $modeMock.Usbip=$false;Reject {Invoke-XoneModeSwitch $fixture Start} 'Missing USBip accepted';$modeMock.Usbip=$true
    $modeMock.Task=@{Actions=@(@{Execute='C:\Other\xone-background.exe';Arguments=''})}
    Reject {Invoke-XoneModeSwitch $fixture Stop} 'Foreign scheduled task accepted';$modeMock.Task=$null
    Write-XoneJson (Join-Path $fixture 'config/app.local.json') @{Version=1;OwnerSid='different-account';AutoStart=$false}
    Reject {Invoke-XoneModeSwitch $fixture Stop} 'Different Windows account accepted'
    Write-Output 'PASS owner, selected adapter, backup, driver, device readiness, prerequisites and foreign-task guards; no real drivers/tasks touched'
}finally{
    $resolved=[IO.Path]::GetFullPath($fixture)
    $boundary=[IO.Path]::GetFullPath((Join-Path $taskRoot 'work'))+[IO.Path]::DirectorySeparatorChar
    if(-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup outside work refused'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
    Remove-Variable -Name modeMock -Scope Global -ErrorAction SilentlyContinue
}
