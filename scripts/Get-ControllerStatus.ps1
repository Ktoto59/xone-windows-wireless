# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([switch]$Json)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
if(Get-XoneMemoryHost $taskRoot){
    $taskMemory=Get-XoneRuntimeState $taskRoot
    if(-not $taskMemory.Runtime -or -not (Test-XoneProcess $taskMemory.Runtime.Worker)){throw 'Bridge is not running.'}
    if($Json){$taskMemory | ConvertTo-Json -Depth 10;return}
    Write-Output ('Phase: '+$taskMemory.Host.Phase)
    $taskMemory.Radio.Controllers | Format-List
    return
}
$taskState=$null
$taskRawDevices=@()
foreach($taskStateName in @('usb-gip-running.json','bridge-running.json')){
    $taskCandidatePath=Join-Path $taskRoot ('work\'+$taskStateName)
    if(-not (Test-Path -LiteralPath $taskCandidatePath)){continue}
    $taskCandidate=Get-Content -LiteralPath $taskCandidatePath -Raw | ConvertFrom-Json
    if($taskStateName -eq 'usb-gip-running.json'){
        if(-not $taskCandidate.Running -or -not $taskCandidate.Radio){continue}
        $taskIdentity=$taskCandidate.Radio
    }else{$taskIdentity=$taskCandidate}
    $taskProcess=Get-Process -Id $taskIdentity.Pid -ErrorAction SilentlyContinue
    if($taskProcess -and $taskProcess.ProcessName -eq 'xone-probe' -and $taskProcess.StartTime.ToUniversalTime().Ticks -eq [long]$taskIdentity.StartTicks){
        $taskState=if($taskStateName -eq 'usb-gip-running.json'){@{StatusFile=$taskCandidate.ControllerStatus}}else{$taskCandidate}
        if($taskStateName -eq 'usb-gip-running.json'){$taskRawDevices=@($taskCandidate.Devices)}
        break
    }
}
if(-not $taskState){throw 'Bridge is not running.'}
if(-not $taskState.StatusFile -or -not (Test-Path -LiteralPath $taskState.StatusFile)){throw 'No live status yet. Start the latest bridge build.'}
$taskStatus=Get-Content -LiteralPath $taskState.StatusFile -Raw | ConvertFrom-Json
foreach($taskClient in $taskStatus.Controllers){
    $taskRawDevice=$taskRawDevices | Where-Object Mac -eq $taskClient.Mac | Select-Object -First 1
    if(-not $taskRawDevice -or -not (Test-Path -LiteralPath $taskRawDevice.Status)){continue}
    try{$taskRadioStatus=Get-Content -LiteralPath $taskRawDevice.Status -Raw | ConvertFrom-Json}catch{continue}
    if($taskRadioStatus.Ready -and $taskRadioStatus.Wcid -eq $taskClient.Wcid -and $taskRadioStatus.Generation -eq $taskRawDevice.Generation -and $taskRadioStatus.RadioBattery){
        $taskClient.Battery=$taskRadioStatus.RadioBattery
        $taskPowerType=[int]$taskClient.Battery.RawType;$taskPowerLevel=[int]$taskClient.Battery.RawLevel
        if($taskPowerType -lt 0 -or $taskPowerType -gt 3 -or $taskPowerLevel -lt 0 -or $taskPowerLevel -gt 3){$taskClient.Battery=$null;continue}
        $taskClient.Battery.Type=@('none','standard','play-and-charge','unknown')[$taskPowerType]
        $taskClient.Battery.Level=if($taskPowerType){@('low','normal','high','full')[$taskPowerLevel]}else{'unknown'}
        $taskClient | Add-Member -NotePropertyName BatterySource -NotePropertyValue 'radio-status' -Force
        $taskClient | Add-Member -NotePropertyName StatusForwarding -NotePropertyValue 'raw-gip-passthrough' -Force
    }
}
if($Json){$taskStatus | ConvertTo-Json -Depth 5;return}
Write-Output ('Backend: '+$taskStatus.Backend)
if($taskStatus.Backend -eq 'usb-gip'){Write-Output ('Other devices output: '+$taskStatus.FallbackOutput)}
Write-Output ('Pairing: '+$taskStatus.Pairing)
Write-Output 'Synthetic output does not publish decoded Share/battery. Raw USB GIP forwards device reports unchanged.'
if(-not $taskStatus.Controllers){Write-Output 'No radio clients connected.';return}
$taskStatus.Controllers | Select-Object Wcid,Mac,Kind,Encrypted,Authentication,AuthVersion,PeerAuthVersion,SupportsShare,SupportsDli,SharePressed,@{Name='BatteryType';Expression={if($_.Battery){$_.Battery.Type}else{'not-reported'}}},@{Name='BatteryLevel';Expression={if($_.Battery){$_.Battery.Level}else{'not-reported'}}},Audio | Format-List
