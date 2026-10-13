# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([switch]$Stop,[ValidateSet('vigem','native-gip')][string]$Backend='native-gip')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Device-Config.ps1')
$taskAdapterId=Resolve-XoneAdapterInstance
[void][IO.Directory]::CreateDirectory((Join-Path $taskRoot 'work'))
$taskStatePath=Join-Path $taskRoot 'work\bridge-running.json'
if(Test-Path -LiteralPath $taskStatePath) {
    $taskPrevious=Get-Content -LiteralPath $taskStatePath -Raw | ConvertFrom-Json
    $taskExisting=Get-Process -Id $taskPrevious.Pid -ErrorAction SilentlyContinue
    if($taskExisting -and $taskExisting.ProcessName -eq 'xone-probe' -and $taskExisting.StartTime.ToUniversalTime().Ticks -eq [long]$taskPrevious.StartTicks) {
        if($Stop){
            Set-Content -LiteralPath $taskPrevious.StopFile -Value 'stop requested'
            if(-not $taskExisting.WaitForExit(5000)){throw 'Bridge is still stopping; inspect its logs before restarting.'}
            Write-Output 'Bridge stopped; virtual controllers removed.';return
        }
        if($taskPrevious.Backend -and $taskPrevious.Backend -ne $Backend){throw "Bridge already uses $($taskPrevious.Backend). Stop it before selecting $Backend."}
        Write-Output "Bridge already running: PID $($taskPrevious.Pid)";return
    }
}
if($Stop){Write-Output 'Bridge is not running.';return}
if($Backend -eq 'native-gip'){
    $taskDevMode=(Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -ErrorAction SilentlyContinue).AllowDevelopmentWithoutDevLicense
    if($taskDevMode -ne 1){throw 'Enable Windows Developer Mode for XboxGipSynthetic, or choose -Backend vigem.'}
    $taskPrincipal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if(-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
        Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSCommandPath+'"'),'-Backend','native-gip') | Out-Null
        Write-Output 'Native GIP bridge start requested with administrator privileges.';return
    }
}
$taskService=(Get-PnpDeviceProperty -InstanceId $taskAdapterId -KeyName 'DEVPKEY_Device_Service').Data
if($taskService -ne 'WinUSB'){throw 'Bind the adapter with Switch-WinUSB.ps1 -Mode Bind and reconnect USB first.'}
$taskStamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$taskStop=Join-Path $taskRoot "work\bridge-$taskStamp.stop"
$taskLog=Join-Path $taskRoot "work\bridge-$taskStamp.log"
$taskError=Join-Path $taskRoot "work\bridge-$taskStamp.err"
$taskFirmware=Join-Path $taskRoot 'work\firmware\FW_ACC_00U.bin'
$taskStatus=Join-Path $taskRoot 'work\controller-status.json'
$taskArgs='run --firmware "'+$taskFirmware+'" --stop-file "'+$taskStop+'" --status-file "'+$taskStatus+'" --output '+$Backend
$taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'outputs\xone-probe.exe') -WorkingDirectory $taskRoot -ArgumentList $taskArgs -RedirectStandardOutput $taskLog -RedirectStandardError $taskError -WindowStyle Hidden -PassThru
$taskState=[ordered]@{Pid=$taskProcess.Id;StartTicks=$taskProcess.StartTime.ToUniversalTime().Ticks;Backend=$Backend;StopFile=$taskStop;Log=$taskLog;ErrorLog=$taskError;StatusFile=$taskStatus}
$taskState | ConvertTo-Json | Set-Content -LiteralPath $taskStatePath -Encoding UTF8
Start-Sleep -Milliseconds 500
$taskProcess.Refresh()
if($taskProcess.HasExited){throw "Bridge exited during startup (code $($taskProcess.ExitCode)). See $taskError and $taskLog."}
$taskState | ConvertTo-Json
