# SPDX-License-Identifier: GPL-2.0-or-later
# Development launcher: one explicit UAC launch, no elevated scheduled task.
[CmdletBinding()]
param([switch]$Stop,[switch]$InternalRun)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Native-Config.ps1')
[void][IO.Directory]::CreateDirectory((Join-Path $taskRoot 'work'))
$taskStatePath=Join-Path $taskRoot 'work/native-gip-running.json'
$taskStopPath=Join-Path $taskRoot 'work/native-gip.stop'
$taskOld=Read-XoneJson $taskStatePath
if($Stop){
    [IO.File]::WriteAllText($taskStopPath,'stop')
    $deadline=[datetime]::UtcNow.AddSeconds(40)
    while($taskOld -and (Test-XoneProcess $taskOld.Supervisor) -and [datetime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 100}
    if($taskOld -and (Test-XoneProcess $taskOld.Supervisor)){throw 'Native GIP is still stopping; its state was preserved.'}
    return
}
if($taskOld -and (Test-XoneProcess $taskOld.Supervisor)){Write-Output 'Direct GIP is already running.';return}
$taskAdapter=Resolve-XoneAdapterInstance
$taskConfig=Initialize-XoneNativeConfig $taskRoot $taskAdapter
if(-not (Test-XoneFirmware $taskRoot)){throw 'Import the adapter RAM firmware before starting direct GIP.'}
if((Get-PnpDeviceProperty -InstanceId $taskAdapter -KeyName DEVPKEY_Device_Service).Data -ne 'WinUSB'){throw 'Direct GIP requires the existing WinUSB adapter binding.'}
foreach($name in @('xone-native.exe','native-gip-probe.exe','native-audio-probe.exe')){if(-not [IO.File]::Exists((Join-Path $taskRoot ('outputs/direct-gip/'+$name)))){throw 'Build the direct GIP candidate with Build-DirectGip.cmd first.'}}
$taskDeveloperMode=(Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -ErrorAction SilentlyContinue).AllowDevelopmentWithoutDevLicense
if($taskDeveloperMode -ne 1){throw 'Direct GIP currently requires Windows Developer Mode. No Windows policy was changed.'}
if(-not [IO.File]::Exists((Join-Path $env:windir 'System32/xboxgipsynthetic.dll'))){throw 'XboxGipSynthetic is unavailable on this Windows build.'}
$taskPrincipal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    if($InternalRun){throw 'Windows did not grant administrator privileges.'}
    $arguments='-NoProfile -ExecutionPolicy Bypass -File '+(Quote-XonePath $PSCommandPath)+' -InternalRun'
    Start-Process -FilePath (Join-Path $env:windir 'System32/WindowsPowerShell/v1.0/powershell.exe') -ArgumentList $arguments -WorkingDirectory $taskRoot -WindowStyle Hidden -Verb RunAs | Out-Null
    Write-Output 'Direct GIP launch requested. Approve the Windows UAC prompt.'
    return
}
$taskSid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$taskMutex=[Threading.Mutex]::new($false,('Local\xone-direct-gip-'+$taskSid));$taskOwnsMutex=$false
try{$taskOwnsMutex=$taskMutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$taskOwnsMutex=$true}
if(-not $taskOwnsMutex){$taskMutex.Dispose();return}
$taskRadio=$null;$taskFailure=$null;$taskFallback=$false
$taskState=[ordered]@{Running=$false;Backend='native-gip';Phase='Starting';Error=$null;Supervisor=@{Pid=$PID;StartTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks};Radio=$null;StatusFile=(Join-Path $taskRoot 'work/native-gip-status.json');RetryCount=0;UpdatedUtc=$null}
function Save-DirectState([string]$Phase,[string]$Error=''){
    $taskState.Phase=$Phase;$taskState.Error=if($Error){$Error}else{$null};$taskState.UpdatedUtc=[datetime]::UtcNow.ToString('o')
    Write-XoneJson $taskStatePath $taskState
}
try{
    $previous=Read-XoneJson (Join-Path $taskRoot 'work/maintenance-status.json')
    $taskFallback=[bool]($previous -and (Test-XoneProcess $previous.Process))
    & (Join-Path $PSScriptRoot 'Set-AutomaticBridge.ps1') -Mode Stop
    Assert-XoneRadioIdle
    if([IO.File]::Exists($taskStopPath)){Remove-Item -LiteralPath $taskStopPath}
    $taskState.Running=$true;Save-DirectState 'StartingRadio'
    while(-not [IO.File]::Exists($taskStopPath)){
        $taskConfig=Read-XoneNativeConfig (Join-Path $taskRoot 'config/native.local.json')
        $stamp=[datetime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
        $taskState.Log=Join-Path $taskRoot ('work/native-gip-'+$stamp+'.log');$taskState.ErrorLog=Join-Path $taskRoot ('work/native-gip-'+$stamp+'.err')
        if([IO.File]::Exists($taskState.StatusFile)){Remove-Item -LiteralPath $taskState.StatusFile}
        $arguments='run --output native-gip --instance '+(Quote-XonePath $taskConfig.AdapterInstanceId)+' --firmware '+(Quote-XonePath (Join-Path $taskRoot 'work/firmware/FW_ACC_00U.bin'))+' --stop-file '+(Quote-XonePath $taskStopPath)+' --status-file '+(Quote-XonePath $taskState.StatusFile)
        foreach($peer in $taskConfig.Peers){$arguments+=' --native-peer '+$peer}
        $taskRadio=Start-Process -FilePath (Join-Path $taskRoot 'outputs/direct-gip/xone-native.exe') -ArgumentList $arguments -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput $taskState.Log -RedirectStandardError $taskState.ErrorLog -PassThru
        $taskState.Radio=@{Pid=$taskRadio.Id;StartTicks=$taskRadio.StartTime.ToUniversalTime().Ticks};Save-DirectState 'StartingRadio';$started=[datetime]::UtcNow
        while(-not $taskRadio.HasExited){
            $snapshot=Read-XoneJson $taskState.StatusFile;Save-XoneNativePeers $taskRoot $taskConfig $snapshot
            Save-DirectState $(if($snapshot -and $snapshot.Running){'Running'}else{'StartingRadio'})
            if([IO.File]::Exists($taskStopPath)){
                if(-not $taskRadio.WaitForExit(15000)){throw 'Native radio did not stop cleanly. Recovery state was kept.'};break
            }
            Start-Sleep -Milliseconds 2000;$taskRadio.Refresh()
        }
        if([IO.File]::Exists($taskStopPath)){break}
        if(([datetime]::UtcNow-$started).TotalSeconds -ge 60){$taskState.RetryCount=0}
        $taskState.RetryCount++
        if($taskState.RetryCount -ge 3){throw ('Native GIP exited repeatedly (last code '+$taskRadio.ExitCode+'). Inspect its private error log.')}
        Save-DirectState 'Retrying' ('Radio exited with code '+$taskRadio.ExitCode)
        $retryAt=[datetime]::UtcNow.AddSeconds(2*[math]::Pow(2,$taskState.RetryCount-1))
        while([datetime]::UtcNow -lt $retryAt -and -not [IO.File]::Exists($taskStopPath)){Start-Sleep -Milliseconds 200}
    }
}catch{$taskFailure=$_.Exception.Message}
finally{
    [IO.File]::WriteAllText($taskStopPath,'stop')
    if($taskRadio -and -not $taskRadio.HasExited -and -not $taskRadio.WaitForExit(15000)){
        # This is only the child process created above, never a guessed PID.
        $taskRadio.Kill();$taskRadio.WaitForExit();$taskFailure='Native radio required forced cleanup; inspect the private logs.'
    }
    try{Save-XoneNativePeers $taskRoot $taskConfig (Read-XoneJson $taskState.StatusFile)}catch{if(-not $taskFailure){$taskFailure=$_.Exception.Message}}
    $taskState.Running=$false;Save-DirectState $(if($taskFailure){'Failed'}else{'Stopped'}) $taskFailure
    $taskMutex.ReleaseMutex();$taskMutex.Dispose()
    if($taskFailure -and $taskFallback){& (Join-Path $PSScriptRoot 'Set-AutomaticBridge.ps1') -Mode Start}
}
if($taskFailure){throw $taskFailure}
