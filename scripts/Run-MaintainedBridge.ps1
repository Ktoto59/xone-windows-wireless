# SPDX-License-Identifier: GPL-2.0-or-later
# User-session supervisor. Never changes drivers, certificates or boot settings.
[CmdletBinding()]
param([switch]$Stop)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
$taskStatePath=Join-Path $taskRoot 'work/maintenance-status.json'
$taskStopPath=Join-Path $taskRoot 'work/maintenance.stop'
[void][IO.Directory]::CreateDirectory((Join-Path $taskRoot 'work'))
if($Stop){
    Set-Content -LiteralPath $taskStopPath -Value stop
    $taskOld=Read-XoneJson $taskStatePath
    $taskDeadline=(Get-Date).AddSeconds(35)
    while($taskOld -and (Test-XoneProcess $taskOld.Process) -and (Get-Date) -lt $taskDeadline){Start-Sleep -Milliseconds 100}
    if($taskOld -and (Test-XoneProcess $taskOld.Process)){throw 'Background supervisor has not stopped; leave recovery state intact.'}
    Stop-XoneBridge $taskRoot
    return
}
$taskSid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$taskMutex=[Threading.Mutex]::new($false,('Local\xone-wireless-maintenance-'+$taskSid))
$taskOwnsMutex=$false
try{$taskOwnsMutex=$taskMutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$taskOwnsMutex=$true}
if(-not $taskOwnsMutex){$taskMutex.Dispose();exit 0}
if(-not ('Xone.AdapterHealth' -as [type])){Add-Type -Path (Join-Path $taskRoot 'src/adapter_health.cs')}
if(Test-Path -LiteralPath $taskStopPath){Remove-Item -LiteralPath $taskStopPath}
$taskStatus=[ordered]@{Phase='Starting';Error=$null;Process=@{Pid=$PID;StartTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks};WorkerActive=$false;RadioActive=$false;DevicesReady=@();RetryCount=0;UpdatedUtc=$null}
$taskRetryAt=[datetime]::MinValue;$taskLaunchUntil=[datetime]::MinValue
$taskStableSince=$null;$taskLastTick=Get-Date;$taskSawWorker=$false
function Save-MaintenanceStatus([string]$Phase,[string]$Error=''){
    $taskStatus.Phase=$Phase;$taskStatus.Error=if($Error){$Error}else{$null}
    $taskStatus.UpdatedUtc=[datetime]::UtcNow.ToString('o')
    # Status-file contention is observational, not a reason to stop devices.
    try{Write-XoneJson $taskStatePath $taskStatus}catch [IO.IOException]{}catch [UnauthorizedAccessException]{}
}
function Schedule-MaintenanceRetry([string]$Error){
    $taskStatus.RetryCount=[Math]::Min(32,$taskStatus.RetryCount+1)
    $taskDelay=[Math]::Min(30,2*[Math]::Pow(2,[Math]::Min(4,$taskStatus.RetryCount-1)))
    $script:taskRetryAt=(Get-Date).AddSeconds($taskDelay)
    $script:taskStableSince=$null
    Save-MaintenanceStatus 'Retrying' $Error
}
try{
    while(-not (Test-Path -LiteralPath $taskStopPath)){
        try{
            $taskNow=Get-Date
            $taskState=Read-XoneJson (Join-Path $taskRoot 'work/usb-gip-running.json')
            $taskStatus.WorkerActive=[bool]($taskState -and (Test-XoneProcess $taskState.Worker))
            $taskStatus.RadioActive=[bool]($taskState -and (Test-XoneProcess $taskState.Radio))
            $taskStatus.DevicesReady=@()
            $taskConfig=Read-XoneDeviceConfig (Join-Path $taskRoot 'config/devices.local.json')
            $taskAdapter=$null
            if($taskConfig.AdapterInstanceId){$taskAdapter=[Xone.AdapterHealth]::Read($taskConfig.AdapterInstanceId)}
            else{$taskAdapter=[Xone.AdapterHealth]::Find()}
            if(-not $taskAdapter){
                if($taskStatus.WorkerActive -or $taskStatus.RadioActive){Stop-XoneBridge $taskRoot}
                $taskStatus.WorkerActive=$false;$taskStatus.RadioActive=$false
                $taskSawWorker=$false;$taskLaunchUntil=[datetime]::MinValue
                Save-MaintenanceStatus 'WaitingForAdapter'
            }else{
                $taskService=$taskAdapter.Service
                if($taskService -ne 'WinUSB' -or $taskAdapter.Status -ne 'OK'){
                    if($taskStatus.WorkerActive -or $taskStatus.RadioActive){Stop-XoneBridge $taskRoot}
                    $taskStatus.WorkerActive=$false;$taskStatus.RadioActive=$false
                    Save-MaintenanceStatus 'AdapterNeedsSetup' 'Select WinUSB through the control panel; automatic mode never changes a driver.'
                }elseif($taskStatus.WorkerActive){
                    $taskSawWorker=$true;$taskLaunchUntil=[datetime]::MinValue
                    if($taskState.FallbackOutput -ne 'none' -or -not $taskState.NativeController -or $taskState.NewHeadsetOnly){
                        Save-MaintenanceStatus 'OtherSession' 'Stop the manually selected output route before using automatic mode.'
                    }elseif(($taskNow-$taskLastTick).TotalSeconds -gt 15){
                        Stop-XoneBridge $taskRoot
                        $taskStatus.WorkerActive=$false;$taskStatus.RadioActive=$false
                        $taskSawWorker=$false
                        $taskRetryAt=[datetime]::MinValue;$taskStableSince=$null
                        Save-MaintenanceStatus 'RecoveringAfterResume'
                    }else{
                        foreach($taskDevice in $taskState.Devices){
                            $taskPeer=Read-XoneJson $taskDevice.Status
                            if($taskDevice.Port -and $taskPeer -and $taskPeer.Ready -and $taskPeer.Generation -eq $taskDevice.Generation){$taskStatus.DevicesReady+=,[string]$taskDevice.Name}
                        }
                        if($taskStatus.RadioActive){
                            if(-not $taskStableSince){$taskStableSince=$taskNow}
                            if(($taskNow-$taskStableSince).TotalSeconds -ge 30){$taskStatus.RetryCount=0}
                            Save-MaintenanceStatus $(if($taskStatus.DevicesReady.Count){'DevicesReady'}else{'WaitingForDevices'})
                        }else{Save-MaintenanceStatus 'StartingRadio'}
                    }
                }elseif($taskStatus.RadioActive){
                    # No takeover or PID/USBip-port guessing after an orphaned worker.
                    Save-MaintenanceStatus 'CleanupRequired' 'The previous radio owner is still alive. Stop that session before restarting.'
                }elseif($taskSawWorker){
                    $taskSawWorker=$false
                    Schedule-MaintenanceRetry $(if($taskState.Error){$taskState.Error}else{'The bridge worker exited; restarting.'})
                }elseif($taskNow -lt $taskLaunchUntil){Save-MaintenanceStatus 'StartingWorker'}
                elseif($taskLaunchUntil -ne [datetime]::MinValue){
                    $taskLaunchUntil=[datetime]::MinValue
                    Schedule-MaintenanceRetry $(if($taskState.Error){$taskState.Error}else{'The worker did not become ready; retrying.'})
                }
                elseif($taskNow -lt $taskRetryAt){Save-MaintenanceStatus 'Retrying' $taskStatus.Error}
                else{
                    Assert-XoneRadioIdle
                    if(-not (Test-XoneFirmware $taskRoot)){throw 'Prepare the adapter firmware in the control panel.'}
                    $taskUsbip=Get-XoneUsbipStatus
                    if(-not $taskUsbip.Ready){throw 'Prepare the signed USBip dependency in the control panel.'}
                    if($taskState -and $taskState.Error){$taskStatus.Error=$taskState.Error}
                    & (Join-Path $PSScriptRoot 'Run-UsbGip.ps1') -AudioBridge -NativeController -FallbackOutput none -Seconds 0 | Out-Null
                    $taskLaunchUntil=(Get-Date).AddSeconds(15)
                    $taskStableSince=$null
                    Save-MaintenanceStatus 'StartingWorker'
                }
            }
            $taskLastTick=Get-Date
        }catch{Schedule-MaintenanceRetry $_.Exception.Message;$taskLastTick=Get-Date}
        Start-Sleep -Milliseconds 2000
    }
}finally{
    try{
        Stop-XoneBridge $taskRoot
        $taskStatus.WorkerActive=$false;$taskStatus.RadioActive=$false;$taskStatus.DevicesReady=@()
        Save-MaintenanceStatus 'Stopped'
    }
    catch{Save-MaintenanceStatus 'CleanupRequired' $_.Exception.Message}
    $taskMutex.ReleaseMutex();$taskMutex.Dispose()
}
