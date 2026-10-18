# SPDX-License-Identifier: GPL-2.0-or-later
# Manual Start/Stop mode changes only; never called by the native runtime.
function Assert-XoneModeOwner([string]$Root){
    $preferences=Read-XoneJson (Join-Path $Root 'config/app.local.json')
    $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    if(-not $preferences -or $preferences.OwnerSid -ne $sid -or $preferences.AutoStart -isnot [bool]){
        throw 'Run this shortcut as the Windows user who installed xone Wireless. Approve UAC with the same account.'
    }
    return $preferences
}
function Get-XoneModeAdapter([string]$Root,[string]$InstanceId=''){
    $config=Read-XoneJson (Join-Path $Root 'config/devices.local.json')
    $saved=Read-XoneJson (Join-Path $Root 'work/binding-state.json')
    $configured=[string]$config.AdapterInstanceId
    if(-not $configured){throw 'Adapter configuration is missing. Run Setup again.'}
    if($InstanceId -and $InstanceId -ne $configured){throw 'The requested adapter does not belong to this installation.'}
    if($saved -and ($saved.InstanceId -ne $configured -or -not (Test-XoneStockBinding $saved))){
        throw 'Saved driver recovery data does not match this adapter. Keep the recovery files and run Setup again.'
    }
    $selected=Resolve-XoneAdapterInstance $configured
    $service=Get-XoneDeviceService $selected
    $stock=(Get-XoneAdapterPackage $selected).Service
    if($service -notin @('WinUSB',$stock)){throw 'The adapter uses an unexpected driver. Run Setup again.'}
    if($service -eq 'WinUSB' -and -not $saved){throw 'The stock-driver backup is missing. Keep this installation and recover the driver before switching modes.'}
    return [pscustomobject]@{InstanceId=$selected;Service=$service;StockService=$stock}
}
function Test-XoneModeRestartPending([string]$Root){
    $path=Join-Path $Root 'work/setup-restart-required.json'
    if(-not [IO.File]::Exists($path)){return $false}
    $boot=(Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime.ToUniversalTime()
    if([IO.File]::GetLastWriteTimeUtc($path) -lt $boot){[IO.File]::Delete($path);return $false}
    return $true
}
function Set-XoneAdapterMode([string]$Root,[ValidateSet('Bridge','Stock')][string]$Mode,[string]$InstanceId=''){
    [void](Assert-XoneModeOwner $Root)
    $adapter=Get-XoneModeAdapter $Root $InstanceId
    $expected=if($Mode -eq 'Bridge'){'WinUSB'}else{$adapter.StockService}
    if($adapter.Service -eq $expected){
        $device=Get-PnpDevice -InstanceId $adapter.InstanceId -PresentOnly -ErrorAction Stop
        if($device.Status -ne 'OK'){throw 'The selected adapter is not ready. Reconnect it or restart Windows and retry.'}
        return [pscustomobject]@{Changed=$false;ReconnectRequired=$false;RestartRequired=(Test-XoneModeRestartPending $Root)}
    }
    if($Mode -eq 'Bridge'){
        if(-not (Test-XoneFirmware $Root) -or -not (Get-XoneUsbipStatus).Ready){throw 'The firmware or USBip dependency is not ready. Run Setup again.'}
    }
    Stop-XoneManagedBridge $Root | Out-Host
    Assert-XoneRadioIdle
    $operation=if($Mode -eq 'Bridge'){'Bind'}else{'Restore'}
    & (Join-Path $Root 'scripts/Switch-WinUSB.ps1') -Mode $operation -InstanceId $adapter.InstanceId | Out-Host
    $restartCode=Invoke-XonePnputil @('/restart-device',$adapter.InstanceId)
    if($restartCode -eq 3010){Write-XoneJson (Join-Path $Root 'work/setup-restart-required.json') @{Required=$true;Reason='Manual adapter mode switch'}}
    if($restartCode -notin @(0,3010)){
        return [pscustomobject]@{Changed=$true;ReconnectRequired=$true;RestartRequired=$false}
    }
    if(Test-XoneModeRestartPending $Root){return [pscustomobject]@{Changed=$true;ReconnectRequired=$false;RestartRequired=$true}}
    $deadline=[datetime]::UtcNow.AddSeconds(15)
    do{
        $device=Get-PnpDevice -InstanceId $adapter.InstanceId -PresentOnly -ErrorAction SilentlyContinue
        if($device -and $device.Status -eq 'OK' -and (Get-XoneDeviceService $adapter.InstanceId) -eq $expected){
            return [pscustomobject]@{Changed=$true;ReconnectRequired=$false;RestartRequired=$false}
        }
        Start-Sleep -Milliseconds 250
    }while([datetime]::UtcNow -lt $deadline)
    throw 'Windows has not confirmed the selected driver. Reconnect the adapter or restart Windows, then retry the shortcut.'
}
function Invoke-XoneModeMaintenance([string]$Root,[ValidateSet('Bridge','Stock')][string]$Mode,[string]$InstanceId){
    $path=Join-Path $Root ('work/mode-'+$Mode+'.json')
    # A cancelled elevation must never reuse a previous successful result.
    if([IO.File]::Exists($path)){[IO.File]::Delete($path)}
    $action=if($Mode -eq 'Bridge'){'SwitchToBridge'}else{'SwitchToStock'}
    $arguments='-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File '+(Quote-XonePath (Join-Path $Root 'scripts/Setup-Action.ps1'))+' -Action '+$action+' -InstanceId '+(Quote-XonePath $InstanceId)+' -ResultPath '+(Quote-XonePath $path)
    try{
        $process=Start-Process -FilePath (Get-XoneSystemTool 'WindowsPowerShell/v1.0/powershell.exe') -ArgumentList $arguments -WorkingDirectory $Root -Verb RunAs -WindowStyle Hidden -PassThru -Wait
    }catch{throw ('Windows approval was cancelled or the driver operation could not start. The bridge has not been started. '+$_.Exception.Message)}
    $result=Read-XoneJson $path
    if($process.ExitCode -ne 0 -or -not $result -or -not $result.Success){
        $reason=if($result.Error){$result.Error}else{'No successful driver-operation result was received.'}
        throw $reason
    }
    if(-not $result.AdapterMode){throw 'The driver operation did not return its adapter state.'}
    return $result.AdapterMode
}
function Invoke-XoneModeSwitch([string]$Root,[ValidateSet('Start','Stop')][string]$Mode){
    $preferences=Assert-XoneModeOwner $Root
    $task=Get-ScheduledTask -TaskName ('xone-windows-wireless-'+$preferences.OwnerSid) -ErrorAction SilentlyContinue
    if($task){
        $actions=@($task.Actions)
        $native=Join-Path $Root 'outputs/xone-background.exe'
        $legacy=Get-XoneSystemTool 'WindowsPowerShell/v1.0/powershell.exe'
        $legacyArguments='-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File '+(Quote-XonePath (Join-Path $Root 'scripts/Run-MaintainedBridge.ps1'))
        if($actions.Count -ne 1 -or -not (($actions[0].Execute -eq $native -and -not $actions[0].Arguments) -or ($actions[0].Execute -eq $legacy -and $actions[0].Arguments -eq $legacyArguments))){
            throw 'Automatic startup belongs to another installation. Use its Stop shortcut first.'
        }
    }
    # Stop the owned runtime even if the adapter is unplugged or its backup is
    # missing. Restoration still fails visibly, without touching an unknown device.
    # Retain AutoStart in app.local.json for the next successful manual Start.
    if($Mode -eq 'Stop'){& (Join-Path $Root 'scripts/Set-AutomaticBridge.ps1') -Mode Disable | Out-Host}
    $adapter=Get-XoneModeAdapter $Root
    if($Mode -eq 'Start' -and (Test-XoneModeRestartPending $Root)){throw 'Windows requests a restart to finish adapter setup. Restart Windows, then retry Start xone Wireless.'}
    if($Mode -eq 'Start' -and (-not (Test-XoneFirmware $Root) -or -not (Get-XoneUsbipStatus).Ready)){
        throw 'The firmware or USBip dependency is not ready. Run Setup again.'
    }
    $target=if($Mode -eq 'Start'){'Bridge'}else{'Stock'}
    $expected=if($target -eq 'Bridge'){'WinUSB'}else{$adapter.StockService}
    $result=if($adapter.Service -eq $expected){Set-XoneAdapterMode $Root $target $adapter.InstanceId}else{Invoke-XoneModeMaintenance $Root $target $adapter.InstanceId}
    if($result.RestartRequired){throw 'Windows requests a restart to finish the driver change. Restart Windows, then retry this shortcut.'}
    if($result.ReconnectRequired){throw 'Reconnect the Xbox adapter to the same USB port, then retry this shortcut. The bridge has not been started.'}
    # Recheck in the caller after UAC, including the installation/adapter identity.
    $confirmed=Get-XoneModeAdapter $Root $adapter.InstanceId
    if($confirmed.Service -ne $expected){throw 'The selected driver could not be verified. The bridge has not been started.'}
    if($Mode -eq 'Start'){
        if($preferences.AutoStart){& (Join-Path $Root 'scripts/Set-AutomaticBridge.ps1') -Mode Enable -NoStart | Out-Host}
        & (Join-Path $Root 'scripts/Set-AutomaticBridge.ps1') -Mode Start | Out-Host
    }
    return [pscustomobject]@{Mode=$target;Changed=[bool]$result.Changed}
}
