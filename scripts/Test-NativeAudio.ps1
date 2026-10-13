# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskPrincipal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSCommandPath+'"')) | Out-Null
    Write-Output 'Native audio test requested with administrator privileges.'
    return
}
$taskResult=[ordered]@{Success=$false;Stage='starting';ExitCode=$null;Error=$null;Started=(Get-Date).ToString('o')}
$taskResultPath=Join-Path $taskRoot 'work\native-audio-test.json'
try {
    $taskResult | ConvertTo-Json | Set-Content -LiteralPath $taskResultPath -Encoding UTF8
    $taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'outputs\native-audio-probe.exe') -WorkingDirectory $taskRoot -RedirectStandardOutput (Join-Path $taskRoot 'work\native-audio-probe.log') -RedirectStandardError (Join-Path $taskRoot 'work\native-audio-probe.err') -WindowStyle Hidden -PassThru
    $null=$taskProcess.Handle # Keep the process handle before a short-lived child exits.
    if(-not $taskProcess.WaitForExit(30000)){
        $taskProcess.Kill()
        throw 'Native audio probe timed out; its own isolated process was terminated.'
    }
    $taskResult.ExitCode=$taskProcess.ExitCode
    if($taskProcess.ExitCode -ne 0){throw "Native audio probe exit $($taskProcess.ExitCode); see its logs."}
    $taskResult.Success=$true
}catch{$taskResult.Error=$_.Exception.Message}
finally {
    $taskResult.Stage='finished'
    $taskResult | ConvertTo-Json | Set-Content -LiteralPath $taskResultPath -Encoding UTF8
}
