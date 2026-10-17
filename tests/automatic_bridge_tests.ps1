# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$fixture=Join-Path $taskRoot ('work/automatic-test-'+[guid]::NewGuid().ToString('N'))
function Check($Condition,[string]$Message){if(-not $Condition){throw $Message}}
[void][IO.Directory]::CreateDirectory((Join-Path $fixture 'scripts'))
[void][IO.Directory]::CreateDirectory((Join-Path $fixture 'outputs'))
[void][IO.Directory]::CreateDirectory((Join-Path $fixture 'work'))
$target=Join-Path $fixture 'scripts/Set-AutomaticBridge.ps1'
Copy-Item -LiteralPath (Join-Path $taskRoot 'scripts/Set-AutomaticBridge.ps1') -Destination $target
[IO.File]::WriteAllText((Join-Path $fixture 'scripts/Setup-Helpers.ps1'),'function Quote-XonePath($Value){return [char]34+$Value+[char]34}')
[IO.File]::WriteAllText((Join-Path $fixture 'outputs/xone-background.exe'),'fixture; never executed')
$taskMock=[pscustomobject]@{Registered=$null;Existing=$null;Started=0}
function Get-ScheduledTask {param($TaskName,$ErrorAction) return $taskMock.Existing}
function New-ScheduledTaskAction {param($Execute,$WorkingDirectory,$Argument) return [pscustomobject]@{Execute=$Execute;WorkingDirectory=$WorkingDirectory;Arguments=$Argument}}
function New-ScheduledTaskPrincipal {param($UserId,$LogonType,$RunLevel) return [pscustomobject]@{UserId=$UserId;LogonType=$LogonType;RunLevel=$RunLevel}}
function New-ScheduledTaskSettingsSet {param($ExecutionTimeLimit,$MultipleInstances,[switch]$AllowStartIfOnBatteries,[switch]$DontStopIfGoingOnBatteries,[switch]$StartWhenAvailable,$RestartCount,$RestartInterval) return [pscustomobject]@{ExecutionTimeLimit=$ExecutionTimeLimit;MultipleInstances=$MultipleInstances}}
function New-ScheduledTaskTrigger {param([switch]$AtLogOn,$User) return [pscustomobject]@{AtLogOn=[bool]$AtLogOn;User=$User}}
function Register-ScheduledTask {param($TaskName,$Action,$Principal,$Settings,$Description,$Trigger,[switch]$Force)
    $taskMock.Registered=[pscustomobject]@{TaskName=$TaskName;Actions=@($Action);Principal=$Principal;Triggers=@($Trigger);State='Ready'}
    $taskMock.Existing=$taskMock.Registered
}
function Start-ScheduledTask {param($TaskName) $taskMock.Started++}
try{
    & $target -Mode Enable -NoStart | Out-Null
    Check ($null -ne $taskMock.Registered -and $taskMock.Started -eq 0) 'Register-only launched a task'
    Check ($taskMock.Registered.Triggers.Count -eq 1 -and $taskMock.Registered.Triggers[0].AtLogOn) 'Register-only omitted sign-in trigger'
    Check ($taskMock.Registered.Principal.RunLevel -eq 'Limited' -and $taskMock.Registered.Principal.LogonType -eq 'Interactive') 'Startup requested elevated or stored-password credentials'
    Check ($taskMock.Registered.Actions[0].Execute -eq (Join-Path $fixture 'outputs/xone-background.exe')) 'Startup targets a shell instead of native host'
    & $target -Mode Start | Out-Null
    Check ($taskMock.Started -eq 1 -and $taskMock.Registered.Triggers.Count -eq 1) 'Start lost owned sign-in registration'
    $taskMock.Registered=$null;$taskMock.Existing=$null
    & $target -Mode Start | Out-Null
    Check ($taskMock.Started -eq 2 -and $taskMock.Registered.Triggers[0] -eq $null) 'Start without auto-start created a sign-in trigger'
    $taskMock.Existing=[pscustomobject]@{Actions=@([pscustomobject]@{Execute='C:\Other\xone-background.exe';Arguments=''});Triggers=@();State='Ready'}
    $rejected=$false;try{& $target -Mode Enable -NoStart | Out-Null}catch{$rejected=$true}
    Check $rejected 'Register-only overwrote another installation task'
    Check ($taskMock.Started -eq 2) 'Rejected foreign task was launched'
    Write-Output 'PASS limited native sign-in registration, register-only, independent start-now and foreign-task ownership; no real tasks touched'
}finally{
    $resolved=[IO.Path]::GetFullPath($fixture)
    $boundary=[IO.Path]::GetFullPath((Join-Path $taskRoot 'work'))+[IO.Path]::DirectorySeparatorChar
    if(-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup outside work refused'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
