# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([switch]$RestoreStock,[switch]$WaitForReadySignal,[ValidateRange(15,180)][int]$Seconds=90)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Device-Config.ps1')
$taskAdapterId=Resolve-XoneAdapterInstance
[void][IO.Directory]::CreateDirectory((Join-Path $taskRoot 'work'))
$taskWork=Join-Path $taskRoot 'work'
$taskStamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$taskName='XoneStockAuth-'+$taskStamp
$taskEtl=Join-Path $taskWork ($taskName+'.etl')
$taskSnapshots=Join-Path $taskWork ($taskName+'-pnp.jsonl')
$taskResultPath=Join-Path $taskWork 'stock-headset-trace-result.json'
$taskReady=Join-Path $taskWork ($taskName+'.ready')
$taskStarted=$false
$taskResult=[ordered]@{Success=$false;Stage='starting';Error=$null;Pid=$PID;Provider='3a81fad7-a142-4b26-be18-074493a433c1';Flags='0x8000';Etl=$taskEtl;Snapshots=$taskSnapshots;ReadySignal=$taskReady;DurationSeconds=$Seconds;AudioRecordingsSaved=$false;StockBindingRestored=$false}
function Save-TaskResult{$taskResult|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $taskResultPath -Encoding UTF8}
function Read-TaskBinding{
    $taskId=$taskAdapterId
    [ordered]@{InstanceId=$taskId;Service=(Get-PnpDeviceProperty -InstanceId $taskId -KeyName DEVPKEY_Device_Service).Data;Inf=(Get-PnpDeviceProperty -InstanceId $taskId -KeyName DEVPKEY_Device_DriverInfPath).Data;Problem=(Get-PnpDeviceProperty -InstanceId $taskId -KeyName DEVPKEY_Device_ProblemCode).Data}
}
try{
    $taskPrincipal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if(-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Administrator privileges required for ETW and driver binding.'}
    Save-TaskResult
    & (Join-Path $PSScriptRoot 'Run-Bridge.ps1') -Stop
    $taskBinding=Read-TaskBinding
    if($taskBinding.Service -eq 'WinUSB' -and $RestoreStock){
        $taskResult.Stage='restoring-stock';Save-TaskResult
        & (Join-Path $PSScriptRoot 'Switch-WinUSB.ps1') -Mode Restore
        $taskBinding=Read-TaskBinding
        $taskResult.StockBindingRestored=$true
    }
    if($taskBinding.Service -ne 'mt7612US' -or $taskBinding.Problem -ne 0){throw 'Expected healthy mt7612US adapter before capture.'}
    $taskResult.Binding=$taskBinding
    # Security/authentication WPP category only. No USB, network or PCM capture.
    & logman.exe create trace $taskName -p ('{'+$taskResult.Provider+'}') $taskResult.Flags 4 -o $taskEtl -f bincirc -max 32 -bs 64 -nb 8 32 -ets
    if($LASTEXITCODE -ne 0){throw "Cannot start stock GIP authentication trace ($LASTEXITCODE)."}
    $taskStarted=$true
    if($WaitForReadySignal){
        $taskResult.Stage='waiting-for-usb';Save-TaskResult
        $taskDeadline=(Get-Date).AddMinutes(5)
        while(-not (Test-Path -LiteralPath $taskReady)){
            if((Get-Date) -ge $taskDeadline){throw 'Timed out waiting for USB reconnection readiness signal.'}
            Start-Sleep -Milliseconds 250
        }
    }
    $taskResult.Stage='capturing';$taskResult.StartedAt=(Get-Date).ToString('o');Save-TaskResult
    $taskDeadline=(Get-Date).AddSeconds($Seconds)
    while((Get-Date) -lt $taskDeadline){
        $taskDevices=@(Get-PnpDevice -PresentOnly|Where-Object{$_.FriendlyName -match 'Xbox|XBOX' -or $_.InstanceId -like 'USB\VID_045E&PID_02FE\*' -or $_.InstanceId -like '*VID_045E&PID_0B32*'}|Select-Object InstanceId,FriendlyName,Class,Status,Problem)
        [ordered]@{Timestamp=(Get-Date).ToString('o');Devices=$taskDevices}|ConvertTo-Json -Depth 5 -Compress|Add-Content -LiteralPath $taskSnapshots -Encoding UTF8
        Start-Sleep -Milliseconds 500
    }
    $taskResult.Success=$true
}catch{$taskResult.Error=$_.Exception.Message}
finally{
    if($taskStarted){
        & logman.exe stop $taskName -ets
        if($LASTEXITCODE -ne 0){$taskResult.Success=$false;$taskResult.Error='ETW stop failed; inspect logman query -ets.'}
    }
    $taskResult.Stage='finished';$taskResult.FinishedAt=(Get-Date).ToString('o');Save-TaskResult
}
if(-not $taskResult.Success){exit 1}
