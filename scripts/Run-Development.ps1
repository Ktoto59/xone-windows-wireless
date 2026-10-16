# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateSet('calls','lines')][string]$TraceLevel='calls',
      [ValidateRange(0,86400)][int]$Seconds=300,
      [ValidateRange(1,16)][int]$Slots=8,
      [string]$InstanceId='', [string]$PythonPath='',
      [switch]$TakeOverInstalled)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
$taskInstalled=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs/xone-windows-wireless'
$taskControl=Join-Path $taskInstalled 'scripts/Set-AutomaticBridge.ps1'
$taskInstalledState=Read-XoneJson (Join-Path $taskInstalled 'work/maintenance-status.json')
$taskInstalledRunning=$taskInstalledState -and (Test-XoneProcess $taskInstalledState.Process)
if($taskInstalledRunning -and -not $TakeOverInstalled){throw 'The installed bridge is running. Use -TakeOverInstalled to stop it for this session and restore it afterward.'}
$taskPrevious=Read-XoneJson (Join-Path $taskRoot 'work/usb-gip-running.json')
if($taskPrevious -and (Test-XoneProcess $taskPrevious.Worker)){throw 'A bridge already owns this development checkout. Stop that owned session first.'}
$taskAdapter=Resolve-XoneAdapterInstance $InstanceId
if((Get-PnpDeviceProperty -InstanceId $taskAdapter -KeyName DEVPKEY_Device_Service).Data -ne 'WinUSB'){throw 'The adapter must already use WinUSB.'}
if(-not $PythonPath){$PythonPath=(Get-Command python.exe -ErrorAction Stop).Source}
$taskVersion=@(& $PythonPath -c 'import sys; print(sys.version_info >= (3,11))')
if($LASTEXITCODE -ne 0 -or ($taskVersion -join '').Trim() -ne 'True'){throw 'Python 3.11 or newer is required.'}
$taskRadio=Join-Path $taskRoot 'outputs/xone-probe.exe'
$taskUsbip=Join-Path $env:ProgramW6432 'USBip/usbip.exe'
foreach($taskRequired in @($taskRadio,$taskUsbip,(Join-Path $taskRoot 'work/firmware/FW_ACC_00U.bin'))){
    if(-not [IO.File]::Exists($taskRequired)){throw ('Missing development prerequisite: '+$taskRequired+'. See docs/DEVELOPMENT.md.')}
}
$taskDirectory=Join-Path $taskRoot ('work/development/'+[datetime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
[void][IO.Directory]::CreateDirectory($taskDirectory)
$taskOldDirectory=$env:XONE_DEVELOPMENT_LOG_DIR
$taskOldTrace=$env:XONE_DEVELOPMENT_TRACE
$taskChild=$null;$taskRestored=$false;$taskStoppedInstalled=$false
$taskMetadata=[ordered]@{StartedUtc=[datetime]::UtcNow.ToString('o');FinishedUtc=$null;TraceLevel=$TraceLevel;Seconds=$Seconds;Slots=$Slots;RadioSHA256=(Get-FileHash -LiteralPath $taskRadio -Algorithm SHA256).Hash;Python=$PythonPath;ProductionRestored=$false;ExitCode=$null;Error=$null}
Push-Location -LiteralPath $taskRoot
try{
    $taskMetadata.Branch=(& git branch --show-current | Out-String).Trim()
    $taskMetadata.Commit=(& git rev-parse HEAD | Out-String).Trim()
    $taskMetadata.Dirty=[bool]@(& git status --porcelain).Count
    Write-XoneJson (Join-Path $taskDirectory 'session.json') $taskMetadata
    if($taskInstalledRunning){
        $taskStoppedInstalled=$true
        & $taskControl -Mode Stop | Out-Null
        $taskCheck=Read-XoneJson (Join-Path $taskInstalled 'work/maintenance-status.json')
        if($taskCheck -and (Test-XoneProcess $taskCheck.Process)){throw 'The installed bridge has not stopped.'}
    }
    $env:XONE_DEVELOPMENT_LOG_DIR=$taskDirectory
    $env:XONE_DEVELOPMENT_TRACE=$TraceLevel
    $taskArguments='-u '+(Quote-XonePath (Join-Path $taskRoot 'src/usbip_auto_manager.py'))+' --root '+(Quote-XonePath $taskRoot)+' --instance '+(Quote-XonePath $taskAdapter)+' --usbip '+(Quote-XonePath $taskUsbip)+' --slots '+$Slots
    $taskChild=Start-Process -FilePath $PythonPath -ArgumentList $taskArguments -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput (Join-Path $taskDirectory 'supervisor-stdout.log') -RedirectStandardError (Join-Path $taskDirectory 'supervisor-stderr.log') -PassThru
    # Retain the process handle before a fast startup failure can discard it.
    $taskChildHandle=$taskChild.Handle
    Write-Output ('Development session: '+$taskDirectory)
    Write-Output 'Anomalies: anomalies-*.jsonl; complete trace: supervisor-*.jsonl and worker-*.jsonl.'
    $taskDeadline=if($Seconds){[datetime]::UtcNow.AddSeconds($Seconds)}else{[datetime]::MaxValue}
    while(-not $taskChild.HasExited -and [datetime]::UtcNow -lt $taskDeadline){
        Start-Sleep -Milliseconds 500
        $taskChild.Refresh()
    }
}catch{
    $taskMetadata.Error=$_.Exception.Message
    throw
}finally{
    $env:XONE_DEVELOPMENT_LOG_DIR=$taskOldDirectory
    $env:XONE_DEVELOPMENT_TRACE=$taskOldTrace
    if($taskChild){
        if(-not $taskChild.HasExited){
            [IO.File]::WriteAllText((Join-Path $taskRoot 'work/usb-gip.stop'),'development session finished')
            [void]$taskChild.WaitForExit(30000)
            $taskChild.Refresh()
        }
        if($taskChild.HasExited){
            $taskChild.WaitForExit()
            $taskMetadata.ExitCode=$taskChild.ExitCode
        }
        else{$taskMetadata.Error='Development cleanup is still running; installed bridge was not restarted to avoid competing adapter owners.'}
    }
    $taskRemaining=$false
    if($taskChild -and $taskChild.HasExited){
        $taskFinal=Read-XoneJson (Join-Path $taskRoot 'work/usb-gip-running.json')
        if($taskFinal){
            $taskIdentities=@($taskFinal.Radio)+@($taskFinal.Devices | ForEach-Object {$_.Server})
            foreach($taskIdentity in $taskIdentities){
                if(Test-XoneProcess $taskIdentity){$taskRemaining=$true}
            }
        }
        if($taskRemaining){$taskMetadata.Error='Owned development children are still running; installed bridge was not restarted.'}
    }
    if($taskStoppedInstalled -and (-not $taskChild -or $taskChild.HasExited) -and -not $taskRemaining){
        try{
            & $taskControl -Mode Start | Out-Null
            $taskRestored=$true
        }catch{
            $taskMetadata.Error=([string]$taskMetadata.Error+' Restore: '+$_.Exception.Message).Trim()
        }
    }
    $taskMetadata.ProductionRestored=$taskRestored
    $taskMetadata.FinishedUtc=[datetime]::UtcNow.ToString('o')
    try{Write-XoneJson (Join-Path $taskDirectory 'session.json') $taskMetadata}finally{Pop-Location}
}
if($taskMetadata.Error){throw $taskMetadata.Error}
if($taskMetadata.ExitCode -ne 0){throw ('Development supervisor exited: '+$taskMetadata.ExitCode+'. Inspect '+$taskDirectory)}
