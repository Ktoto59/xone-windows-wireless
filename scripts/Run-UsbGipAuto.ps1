# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([switch]$Stop,[switch]$PairOnStart,[switch]$IdleChannelScan,[switch]$PreserveOldWirelessMode=$true,[ValidateRange(1,16)][int]$Slots=8,[string]$InstanceId='')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Device-Config.ps1')
$taskStatePath=Join-Path $taskRoot 'work/usb-gip-running.json'
$taskStopPath=Join-Path $taskRoot 'work/usb-gip.stop'
function Test-AutoOwner($Identity){
    if(-not $Identity){return $false}
    $taskOwned=Get-Process -Id $Identity.Pid -ErrorAction SilentlyContinue
    return $taskOwned -and $taskOwned.StartTime.ToUniversalTime().Ticks -eq [long]$Identity.StartTicks
}
$taskState=if(Test-Path -LiteralPath $taskStatePath){Get-Content -LiteralPath $taskStatePath -Raw -Encoding UTF8|ConvertFrom-Json}else{$null}
if($taskState -and (Test-AutoOwner $taskState.Worker)){
    if(-not $taskState.Automatic){throw 'A fixed-route bridge is running. Stop it before selecting automatic pairing.'}
    if($Stop){Set-Content -LiteralPath $taskStopPath -Value stop;Write-Output 'Automatic bridge stop requested.';return}
    if($taskState.SlotCapacity -ne $Slots){throw 'Stop the automatic bridge before changing slot capacity.'}
    if([bool]$taskState.IdleChannelScan -ne [bool]$IdleChannelScan){throw 'Stop the automatic bridge before changing channel scan mode.'}
    if([bool]$taskState.PreserveOldWirelessMode -ne [bool]$PreserveOldWirelessMode){throw 'Stop the automatic bridge before changing wireless mode adaptation.'}
    Write-Output 'Automatic bridge is already running.';return
}
if($Stop){Write-Output 'Automatic bridge is not running.';return}
if(-not $InstanceId){
    $taskPrivateConfig=Join-Path $taskRoot 'config/devices.local.json'
    if(Test-Path -LiteralPath $taskPrivateConfig){$InstanceId=(Get-Content -LiteralPath $taskPrivateConfig -Raw -Encoding UTF8|ConvertFrom-Json).AdapterInstanceId}
}
$InstanceId=Resolve-XoneAdapterInstance $InstanceId
if((Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_Service).Data -ne 'WinUSB'){throw 'The adapter must already use WinUSB; this command does not change driver bindings.'}
$taskPython=Join-Path $taskRoot 'runtime/python/python.exe'
if(-not(Test-Path -LiteralPath $taskPython)){$taskPython=(Get-Command python.exe).Source}
$taskUsbip=Join-Path $env:ProgramW6432 'USBip/usbip.exe'
foreach($taskRequired in @($taskUsbip,(Join-Path $taskRoot 'outputs/xone-probe.exe'),(Join-Path $taskRoot 'work/firmware/FW_ACC_00U.bin'))){if(-not(Test-Path -LiteralPath $taskRequired)){throw "Missing prerequisite: $taskRequired"}}
if($InstanceId -match '["\r\n]' -or $taskRoot -match '["\r\n]'){throw 'Invalid argument path.'}
if(Test-Path -LiteralPath $taskStopPath){Remove-Item -LiteralPath $taskStopPath}
$taskArgs='"'+(Join-Path $taskRoot 'src/usbip_auto_manager.py')+'" --root "'+$taskRoot+'" --instance "'+$InstanceId+'" --usbip "'+$taskUsbip+'" --slots '+$Slots
if($PairOnStart){$taskArgs+=' --pair'}
if($IdleChannelScan){$taskArgs+=' --idle-channel-scan'}
if($PreserveOldWirelessMode){$taskArgs+=' --preserve-old-wireless-mode'}else{$taskArgs+=' --forward-old-usb-mode'}
$taskLog=Join-Path $taskRoot 'work/usb-gip-auto-manager.log'
$taskError=Join-Path $taskRoot 'work/usb-gip-auto-manager.err'
$taskProcess=Start-Process -FilePath $taskPython -ArgumentList $taskArgs -WindowStyle Hidden -WorkingDirectory $taskRoot -RedirectStandardOutput $taskLog -RedirectStandardError $taskError -PassThru
Write-Output "Automatic USB GIP requested: PID $($taskProcess.Id); log=$taskLog"
