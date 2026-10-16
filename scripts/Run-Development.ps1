# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateSet('calls','lines')][string]$TraceLevel='calls',
      [ValidateSet('python','cpp')][string]$Backend='python',
      [ValidateRange(0,86400)][int]$Seconds=300,
      [ValidateRange(1,16)][int]$Slots=8,
      [string]$InstanceId='', [string]$PythonPath='',
      [switch]$CppPacketTrace,
      [switch]$NoUsbMmcss,
      [switch]$TakeOverInstalled)
$ErrorActionPreference='Stop'
if($NoUsbMmcss -and $Backend -ne 'cpp'){throw '-NoUsbMmcss requires -Backend cpp.'}
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
$taskInstalled=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs/xone-windows-wireless'
$taskControl=Join-Path $taskInstalled 'scripts/Set-AutomaticBridge.ps1'
$taskInstalledState=Read-XoneJson (Join-Path $taskInstalled 'work/maintenance-status.json')
$taskInstalledRunning=$taskInstalledState -and (Test-XoneProcess $taskInstalledState.Process)
if($taskInstalledRunning -and -not $TakeOverInstalled){throw 'The installed bridge is running. Use -TakeOverInstalled to stop it for this session and restore it afterward.'}
$taskPrevious=Read-XoneJson (Join-Path $taskRoot 'work/usb-gip-running.json')
if($taskPrevious -and (Test-XoneProcess $taskPrevious.Worker)){throw 'A bridge already owns this development checkout. Stop that owned session first.'}
$taskPreviousHost=Read-XoneJson (Join-Path $taskRoot 'work/maintenance-status.json')
if($taskPreviousHost -and (Test-XoneProcess $taskPreviousHost.Process)){throw 'A C++ host already owns this development checkout. Stop that owned session first.'}
$taskAdapter=Resolve-XoneAdapterInstance $InstanceId
if((Get-PnpDeviceProperty -InstanceId $taskAdapter -KeyName DEVPKEY_Device_Service).Data -ne 'WinUSB'){throw 'The adapter must already use WinUSB.'}
if($Backend -eq 'python'){
    if(-not $PythonPath){$PythonPath=(Get-Command python.exe -ErrorAction Stop).Source}
    $taskVersion=@(& $PythonPath -c 'import sys; print(sys.version_info >= (3,11))')
    if($LASTEXITCODE -ne 0 -or ($taskVersion -join '').Trim() -ne 'True'){throw 'Python 3.11 or newer is required.'}
}elseif($Slots -ne 8){throw 'The C++ host currently uses eight automatic slots.'}
$taskRadio=Join-Path $taskRoot $(if($Backend -eq 'cpp'){'outputs/appliance-candidate/xone-probe-cpp.exe'}else{'outputs/xone-probe.exe'})
$taskHost=Join-Path $taskRoot 'outputs/appliance-candidate/xone-background.exe'
if($Backend -eq 'cpp' -and -not [IO.File]::Exists($taskHost)){throw 'Build the C++ candidate with Build-Appliance.cmd first.'}
$taskUsbip=Join-Path $env:ProgramW6432 'USBip/usbip.exe'
foreach($taskRequired in @($taskRadio,$taskUsbip,(Join-Path $taskRoot 'work/firmware/FW_ACC_00U.bin'))){
    if(-not [IO.File]::Exists($taskRequired)){throw ('Missing development prerequisite: '+$taskRequired+'. See docs/DEVELOPMENT.md.')}
}
$taskDirectory=Join-Path $taskRoot ('work/development/'+[datetime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
[void][IO.Directory]::CreateDirectory($taskDirectory)
$taskOldDirectory=$env:XONE_DEVELOPMENT_LOG_DIR
$taskOldTrace=$env:XONE_DEVELOPMENT_TRACE
$taskOldCppDirectory=$env:XONE_CPP_LOG_DIR
$taskOldCppTrace=$env:XONE_CPP_TRACE
$taskOldUsbMmcss=$env:XONE_USB_MMCSS
$taskChild=$null;$taskRestored=$false;$taskStoppedInstalled=$false
$taskMetadata=[ordered]@{StartedUtc=[datetime]::UtcNow.ToString('o');FinishedUtc=$null;Backend=$Backend;TraceLevel=$TraceLevel;CppPacketTrace=[bool]$CppPacketTrace;UsbMmcssDisabled=([bool]$NoUsbMmcss -or $taskOldUsbMmcss -eq '0');Seconds=$Seconds;Slots=$Slots;RadioSHA256=(Get-FileHash -LiteralPath $taskRadio -Algorithm SHA256).Hash;Python=$PythonPath;ProductionRestored=$false;ExitCode=$null;Error=$null}
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
    $env:XONE_CPP_LOG_DIR=$taskDirectory
    $env:XONE_CPP_TRACE=if($CppPacketTrace){'1'}else{'0'}
    if($NoUsbMmcss){$env:XONE_USB_MMCSS='0'}
    $taskArguments='-u '+(Quote-XonePath (Join-Path $taskRoot 'src/usbip_auto_manager.py'))+' --root '+(Quote-XonePath $taskRoot)+' --instance '+(Quote-XonePath $taskAdapter)+' --usbip '+(Quote-XonePath $taskUsbip)+' --slots '+$Slots
    $taskLaunch=@{FilePath=$PythonPath;ArgumentList=$taskArguments;WorkingDirectory=$taskRoot;WindowStyle='Hidden';RedirectStandardOutput=(Join-Path $taskDirectory 'supervisor-stdout.log');RedirectStandardError=(Join-Path $taskDirectory 'supervisor-stderr.log');PassThru=$true}
    if($Backend -eq 'cpp'){$taskLaunch.FilePath=$taskHost;$taskLaunch.ArgumentList='--instance '+(Quote-XonePath $taskAdapter)}
    $taskChild=Start-Process @taskLaunch
    # Retain the process handle before a fast startup failure can discard it.
    $taskChildHandle=$taskChild.Handle
    Write-Output ('Development session: '+$taskDirectory)
    Write-Output ('C++ packet tracing: '+[bool]$CppPacketTrace+'; seconds: '+$Seconds+' (0 = until stopped).')
    Write-Output 'Anomalies: anomalies-*.jsonl; trace: cpp-*.jsonl, supervisor-*.jsonl and worker-*.jsonl.'
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
    $env:XONE_CPP_LOG_DIR=$taskOldCppDirectory
    $env:XONE_CPP_TRACE=$taskOldCppTrace
    $env:XONE_USB_MMCSS=$taskOldUsbMmcss
    if($taskChild){
        if(-not $taskChild.HasExited){
            [IO.File]::WriteAllText((Join-Path $taskRoot 'work/usb-gip.stop'),'development session finished')
            if($Backend -eq 'cpp'){[IO.File]::WriteAllText((Join-Path $taskRoot 'work/maintenance.stop'),'development session finished')}
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
