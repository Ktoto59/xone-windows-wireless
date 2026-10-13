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
$taskLegacyArguments='-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File '+(Quote-XonePath $taskScript)
$taskHostExe=Join-Path $taskRoot 'outputs/xone-background.exe'
$taskExecutable=if(Test-Path -LiteralPath $taskHostExe){$taskHostExe}else{$taskPowerShell}
$taskArguments=if($taskExecutable -eq $taskHostExe){''}else{$taskLegacyArguments}
$taskExisting=Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if($taskExisting){
    $taskActions=@($taskExisting.Actions)
    $taskOwned=$taskActions.Count -eq 1 -and (($taskActions[0].Execute -eq $taskPowerShell -and $taskActions[0].Arguments -eq $taskLegacyArguments) -or ($taskActions[0].Execute -eq $taskHostExe -and -not $taskActions[0].Arguments))
    if(-not $taskOwned){
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
    # The process can exit just before Task Scheduler marks its action stopped.
    # IgnoreNew would otherwise swallow an immediate Start after this Stop.
    if($taskExisting){
        $taskExitDeadline=(Get-Date).AddSeconds(5)
        do{
            $taskStopping=Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
            if(-not $taskStopping -or $taskStopping.State -ne 'Running'){break}
            Start-Sleep -Milliseconds 100
        }while((Get-Date) -lt $taskExitDeadline)
        if($taskStopping -and $taskStopping.State -eq 'Running'){throw 'The scheduled host is still stopping. Retry after it exits.'}
    }
    if($Mode -eq 'Disable' -and $taskExisting){Unregister-ScheduledTask -TaskName $taskName -Confirm:$false}
    return
}
if($Mode -eq 'Enable' -or -not $taskExisting -or $taskExisting.Actions[0].Execute -ne $taskExecutable){
    $taskActionParameters=@{Execute=$taskExecutable;WorkingDirectory=$taskRoot}
    if($taskArguments){$taskActionParameters.Argument=$taskArguments}
    $taskAction=New-ScheduledTaskAction @taskActionParameters
    $taskPrincipal=New-ScheduledTaskPrincipal -UserId $taskSid -LogonType Interactive -RunLevel Limited
    $taskSettings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([timespan]::Zero) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -RestartCount 3 -RestartInterval ([timespan]::FromMinutes(1))
    $taskParameters=@{TaskName=$taskName;Action=$taskAction;Principal=$taskPrincipal;Settings=$taskSettings;Description='xone user-session radio bridge with bounded recovery; no administrator privileges.'}
    if($Mode -eq 'Enable'){$taskParameters.Trigger=New-ScheduledTaskTrigger -AtLogOn -User $taskSid}
    elseif($taskExisting -and $taskExisting.Triggers){$taskParameters.Trigger=@($taskExisting.Triggers)}
    Register-ScheduledTask @taskParameters -Force | Out-Null
}
if(Test-Path -LiteralPath (Join-Path $taskRoot 'work/maintenance.stop')){Remove-Item -LiteralPath (Join-Path $taskRoot 'work/maintenance.stop')}
Start-ScheduledTask -TaskName $taskName
Write-Output 'Background bridge requested. No UAC or driver changes.'
