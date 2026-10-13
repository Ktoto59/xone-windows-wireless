# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateSet('Enable','Disable','Start','Stop','Status')][string]$Mode='Status')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
$taskSid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$taskName='xone-windows-wireless-'+$taskSid
$taskScript=Join-Path $taskRoot 'scripts/Run-MaintainedBridge.ps1'
$taskPowerShell=Join-Path $env:windir 'System32/WindowsPowerShell/v1.0/powershell.exe'
$taskArguments='-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File '+(Quote-XonePath $taskScript)
$taskExisting=Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if($taskExisting){
    $taskActions=@($taskExisting.Actions)
    if($taskActions.Count -ne 1 -or $taskActions[0].Execute -ne $taskPowerShell -or $taskActions[0].Arguments -ne $taskArguments){
        if($Mode -in @('Stop','Disable')){& $taskScript -Stop;return}
        throw 'Automatic mode belongs to another installation. Disable it from that installation first.'
    }
}
if($Mode -eq 'Status'){
    [pscustomobject]@{Registered=[bool]$taskExisting;TaskName=$taskName;AutoStart=[bool]($taskExisting -and @($taskExisting.Triggers | Where-Object {$null -ne $_}).Count);State=if($taskExisting){[string]$taskExisting.State}else{'NotRegistered'}}
    return
}
if($Mode -in @('Stop','Disable')){
    & $taskScript -Stop
    if($Mode -eq 'Disable' -and $taskExisting){Unregister-ScheduledTask -TaskName $taskName -Confirm:$false}
    return
}
if($Mode -eq 'Enable' -or -not $taskExisting){
    $taskAction=New-ScheduledTaskAction -Execute $taskPowerShell -Argument $taskArguments -WorkingDirectory $taskRoot
    $taskPrincipal=New-ScheduledTaskPrincipal -UserId $taskSid -LogonType Interactive -RunLevel Limited
    $taskSettings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([timespan]::Zero) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -RestartCount 3 -RestartInterval ([timespan]::FromMinutes(1))
    $taskParameters=@{TaskName=$taskName;Action=$taskAction;Principal=$taskPrincipal;Settings=$taskSettings;Description='xone user-session radio bridge with bounded recovery; no administrator privileges.'}
    if($Mode -eq 'Enable'){$taskParameters.Trigger=New-ScheduledTaskTrigger -AtLogOn -User $taskSid}
    Register-ScheduledTask @taskParameters -Force | Out-Null
}
if(Test-Path -LiteralPath (Join-Path $taskRoot 'work/maintenance.stop')){Remove-Item -LiteralPath (Join-Path $taskRoot 'work/maintenance.stop')}
Start-ScheduledTask -TaskName $taskName
Write-Output 'Background bridge requested. No UAC or driver changes.'
