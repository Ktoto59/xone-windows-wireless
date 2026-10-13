# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([switch]$EnableDeveloperMode, [int]$SessionSeconds=900)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskResultPath=Join-Path $taskRoot 'work\native-gip-test.json'
$taskResult=[ordered]@{Success=$false;Stage='starting';Error=$null;Slot=$null;Created=$false;Connected=$false;Removed=$false}
try {
    if($EnableDeveloperMode){
        $taskKey='HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
        $taskOld=Get-ItemProperty -LiteralPath $taskKey -ErrorAction SilentlyContinue
        $taskBackup=Join-Path $taskRoot 'work\developer-mode-before.json'
        if(-not (Test-Path -LiteralPath $taskBackup)){
            [ordered]@{Key=$taskKey;Property='AllowDevelopmentWithoutDevLicense';Present=($null -ne $taskOld -and $null -ne $taskOld.PSObject.Properties['AllowDevelopmentWithoutDevLicense']);Value=$taskOld.AllowDevelopmentWithoutDevLicense} | ConvertTo-Json | Set-Content -LiteralPath $taskBackup -Encoding UTF8
        }
        New-Item -Path $taskKey -Force | Out-Null
        New-ItemProperty -LiteralPath $taskKey -Name AllowDevelopmentWithoutDevLicense -PropertyType DWord -Value 1 -Force | Out-Null
        $taskResult.Stage='developer-mode-enabled'
    }
    $taskLog=Join-Path $taskRoot 'work\native-gip-probe.log'
    $taskError=Join-Path $taskRoot 'work\native-gip-probe.err'
    $taskResult.Stage='native-probe'
    $taskResult | ConvertTo-Json | Set-Content -LiteralPath $taskResultPath -Encoding UTF8
    $taskProbe=Start-Process -FilePath (Join-Path $taskRoot 'outputs\native-gip-probe.exe') -RedirectStandardOutput $taskLog -RedirectStandardError $taskError -WindowStyle Hidden -PassThru -Wait
    $taskOutput=Get-Content -LiteralPath $taskLog -Raw
    $taskResult.Created=($taskOutput -match 'Create HRESULT=0x0\b')
    $taskResult.Connected=($taskOutput -match 'Connect HRESULT=0x0\b')
    $taskResult.Removed=($taskOutput -match 'Remove HRESULT=0x0\b')
    if($taskOutput -match 'PASS native GIP XInput slot=(\d+)'){$taskResult.Slot=[int]$Matches[1]}
    if($taskProbe.ExitCode -ne 0){throw "Native probe failed ($($taskProbe.ExitCode)). See $taskLog and $taskError."}
    $taskResult.Success=$true
} catch {$taskResult.Error=$_.Exception.Message}
finally {
    $taskResult.Stage='finished'
    $taskResult | ConvertTo-Json | Set-Content -LiteralPath $taskResultPath -Encoding UTF8
    $taskResult | ConvertTo-Json
}
if(-not $taskResult.Created){exit 1}
# Keep this elevated test session available for fixed project test/bridge actions.
# Requests never contain shell commands, executable paths or arbitrary arguments.
$taskRequestPath=Join-Path $taskRoot 'work\native-session-request.json'
$taskResponsePath=Join-Path $taskRoot 'work\native-session-response.json'
$taskSessionPid=Join-Path $taskRoot 'work\native-session-pid.txt'
Set-Content -LiteralPath $taskSessionPid -Value $PID
$taskSessionDeadline=(Get-Date).AddSeconds($SessionSeconds)
while((Get-Date) -lt $taskSessionDeadline){
    if(-not (Test-Path -LiteralPath $taskRequestPath)){Start-Sleep -Milliseconds 200;continue}
    $taskRequest=Get-Content -LiteralPath $taskRequestPath -Raw | ConvertFrom-Json
    Remove-Item -LiteralPath $taskRequestPath
    $taskResponse=[ordered]@{Id=$taskRequest.Id;Success=$false;Error=$null}
    try {
        switch($taskRequest.Action){
            'probe' {
                $taskProbe=Start-Process -FilePath (Join-Path $taskRoot 'outputs\native-gip-probe.exe') -RedirectStandardOutput (Join-Path $taskRoot 'work\native-gip-probe.log') -RedirectStandardError (Join-Path $taskRoot 'work\native-gip-probe.err') -WindowStyle Hidden -PassThru -Wait
                if($taskProbe.ExitCode -ne 0){throw "Probe exit $($taskProbe.ExitCode)"}
            }
            'bridge-start' { & (Join-Path $PSScriptRoot 'Run-Bridge.ps1') -Backend native-gip }
            'backend-test' {
                $taskProbe=Start-Process -FilePath (Join-Path $taskRoot 'outputs\native-gip-probe.exe') -ArgumentList '--backend-smoke' -RedirectStandardOutput (Join-Path $taskRoot 'work\native-backend-smoke.log') -RedirectStandardError (Join-Path $taskRoot 'work\native-backend-smoke.err') -WindowStyle Hidden -PassThru -Wait
                if($taskProbe.ExitCode -ne 0){throw "Backend probe exit $($taskProbe.ExitCode)"}
            }
            'bridge-stop' { & (Join-Path $PSScriptRoot 'Run-Bridge.ps1') -Stop }
            'stop' {$taskSessionDeadline=Get-Date}
            default {throw 'Unknown fixed test action.'}
        }
        $taskResponse.Success=$true
    }catch{$taskResponse.Error=$_.Exception.Message}
    $taskResponse | ConvertTo-Json | Set-Content -LiteralPath $taskResponsePath -Encoding UTF8
}
