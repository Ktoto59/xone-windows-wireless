# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([switch]$Stop,[switch]$Worker,[switch]$AudioProbe,[switch]$AudioBridge,[switch]$NewHeadsetOnly,[switch]$NativeController,[string]$ConfigPath='',[ValidateScript({$_ -eq 0 -or ($_ -ge 30 -and $_ -le 86400)})][int]$Seconds=1800,[ValidateSet('none','native-gip','vigem')][string]$FallbackOutput='none')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Device-Config.ps1')
[void][IO.Directory]::CreateDirectory((Join-Path $taskRoot 'work'))
if(-not $ConfigPath){$ConfigPath=Join-Path $taskRoot 'config/devices.local.json'}
$ConfigPath=[IO.Path]::GetFullPath($ConfigPath)
if($ConfigPath -match '[\r\n"]'){throw 'Invalid configuration path.'}
$taskStatePath=Join-Path $taskRoot 'work\usb-gip-running.json'
$taskStopPath=Join-Path $taskRoot 'work\usb-gip.stop'
function Test-TaskProcess($State){
    if(-not $State){return $false}
    $taskProcess=Get-Process -Id $State.Pid -ErrorAction SilentlyContinue
    return $taskProcess -and $taskProcess.StartTime.ToUniversalTime().Ticks -eq [long]$State.StartTicks
}
function Set-TaskMediaPriority($Process){
    # Scheduled background hosts can start children below normal priority.
    # Audio and RF deadlines need preference over ordinary CPU-bound work.
    try{$Process.PriorityClass=[Diagnostics.ProcessPriorityClass]::AboveNormal}
    catch{Write-Output ('Could not set media priority: '+$_.Exception.Message)}
}
if(-not $Worker){
    if(Test-Path -LiteralPath $taskStatePath){
        $taskOld=Get-Content -LiteralPath $taskStatePath -Raw | ConvertFrom-Json
        if(Test-TaskProcess $taskOld.Worker){
            if($Stop){Set-Content -LiteralPath $taskStopPath -Value stop;Write-Output 'USB GIP stop requested.';return}
            if($taskOld.FallbackOutput -ne $FallbackOutput -or [bool]$taskOld.NewHeadsetOnly -ne [bool]$NewHeadsetOnly -or [bool]$taskOld.NativeController -ne [bool]$NativeController){throw 'Stop the existing USB GIP worker before changing output routes.'}
            Write-Output 'USB GIP worker already running.';return
        }
    }
    if($Stop){Write-Output 'USB GIP worker is not running.';return}
    $taskConfig=Read-XoneDeviceConfig $ConfigPath
    $taskAdapterId=Resolve-XoneAdapterInstance $taskConfig.AdapterInstanceId
    if((Get-PnpDeviceProperty -InstanceId $taskAdapterId -KeyName DEVPKEY_Device_Service).Data -ne 'WinUSB'){throw 'Select WinUSB and reconnect the physical adapter first.'}
    $taskArgs='-NoProfile -ExecutionPolicy Bypass -File "'+$PSCommandPath+'" -Seconds '+$Seconds+' -FallbackOutput '+$FallbackOutput+' -ConfigPath "'+$ConfigPath+'"'
    if($AudioProbe){$taskArgs+=' -AudioProbe'}
    if($AudioBridge){$taskArgs+=' -AudioBridge'}
    if($NewHeadsetOnly){$taskArgs+=' -NewHeadsetOnly'}
    if($NativeController){$taskArgs+=' -NativeController'}
    if($FallbackOutput -eq 'native-gip'){
        $taskDevMode=(Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -ErrorAction SilentlyContinue).AllowDevelopmentWithoutDevLicense
        if($taskDevMode -ne 1){throw 'Enable Developer Mode before using native-gip fallback.'}
        $taskPrincipal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
        if(-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
            Start-Process -FilePath powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList $taskArgs | Out-Null
            Write-Output 'USB GIP mixed-device start requested with administrator privileges.';return
        }
    }
    if(Test-Path -LiteralPath $taskStopPath){Remove-Item -LiteralPath $taskStopPath}
    $taskLog=Join-Path $taskRoot 'work\usb-gip-worker.log'
    $taskError=Join-Path $taskRoot 'work\usb-gip-worker.err'
    $taskProcess=Start-Process -FilePath powershell.exe -ArgumentList ($taskArgs+' -Worker') -WindowStyle Hidden -WorkingDirectory $taskRoot -RedirectStandardOutput $taskLog -RedirectStandardError $taskError -PassThru
    Write-Output "USB GIP worker requested: PID $($taskProcess.Id); log=$taskLog";return
}

$taskRadio=$null;$taskServers=@()
$taskStamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$taskUsbip='C:\Program Files\USBip\usbip.exe'
$taskControllerStatus=Join-Path $taskRoot 'work\usb-gip-controller-status.json'
$taskConfig=Read-XoneDeviceConfig $ConfigPath
$taskAdapterId=Resolve-XoneAdapterInstance $taskConfig.AdapterInstanceId
$taskDefinitions=@($taskConfig.Devices | Where-Object {(-not $NewHeadsetOnly -or $_.ProfileName -eq 'headset-0b32-usb.json') -and ($NativeController -or $_.Audio)})
if(-not $taskDefinitions.Count){throw 'No selected devices. Add a headset profile or use -NativeController.'}
$taskDevices=@($taskDefinitions | ForEach-Object {
    [ordered]@{Name=$_.Name;Mac=$_.Mac;Audio=$_.Audio;UsbPort=$_.UsbPort;RadioPort=$_.RadioPort;Profile=(Join-Path $taskRoot ('driver\profiles\'+$_.ProfileName));Status=(Join-Path $taskRoot ('work\'+$_.StatusName));Server=$null;Port=0;Generation=-1;ServerLog=(Join-Path $taskRoot "work\usb-gip-$($_.Name)-server-$taskStamp.log");ServerError=(Join-Path $taskRoot "work\usb-gip-$($_.Name)-server-$taskStamp.err")}
})
$taskState=[ordered]@{Running=$true;FallbackOutput=$FallbackOutput;NewHeadsetOnly=[bool]$NewHeadsetOnly;NativeController=[bool]$NativeController;ControllerStatus=$taskControllerStatus;Worker=@{Pid=$PID;StartTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks};Devices=$taskDevices;Server=$null;Radio=$null;Port=0;Error=$null;RadioStatus=$taskDevices[0].Status;ServerLog=$taskDevices[0].ServerLog;ServerError=$taskDevices[0].ServerError;RadioLog=(Join-Path $taskRoot "work\usb-gip-radio-$taskStamp.log");RadioError=(Join-Path $taskRoot "work\usb-gip-radio-$taskStamp.err")}
function Save-TaskState{
    $taskState.Server=$taskDevices[0].Server;$taskState.Port=$taskDevices[0].Port
    $taskTemporary=$taskStatePath+'.tmp'
    $taskJson=$taskState | ConvertTo-Json -Depth 7
    for($taskAttempt=0;$taskAttempt -lt 5;++$taskAttempt){
        try{
            [IO.File]::WriteAllText($taskTemporary,$taskJson,[Text.UTF8Encoding]::new($false))
            if([IO.File]::Exists($taskStatePath)){[IO.File]::Replace($taskTemporary,$taskStatePath,[NullString]::Value)}else{[IO.File]::Move($taskTemporary,$taskStatePath)}
            return
        }catch [IO.IOException]{Start-Sleep -Milliseconds 20}
        catch [UnauthorizedAccessException]{Start-Sleep -Milliseconds 20}
    }
    Write-Output 'State snapshot temporarily unavailable; bridge continues.'
}
function Read-TaskRadioState([string]$Path){
    for($taskAttempt=0;$taskAttempt -lt 5;++$taskAttempt){
        $taskStream=$null;$taskReader=$null
        try{
            $taskStream=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
            $taskReader=[IO.StreamReader]::new($taskStream,[Text.Encoding]::UTF8)
            return ($taskReader.ReadToEnd() | ConvertFrom-Json)
        }catch [IO.IOException]{Start-Sleep -Milliseconds 20}
        catch [UnauthorizedAccessException]{Start-Sleep -Milliseconds 20}
        finally{if($taskReader){$taskReader.Dispose()}elseif($taskStream){$taskStream.Dispose()}}
    }
    return $null
}
function Stop-TaskImportedPort($Device){
    if($Device.Port){
        & $taskUsbip detach -p $Device.Port
        if($LASTEXITCODE -ne 0){throw "Could not detach this run's USBip port $($Device.Port)"}
        $Device.Port=0;Save-TaskState
    }
}
function Get-TaskExitError($Process,[string]$Label,[string]$ErrorPath){
    $taskCode=$Process.ExitCode
    $taskDetail=if(Test-Path -LiteralPath $ErrorPath){Get-Content -LiteralPath $ErrorPath -Tail 1}else{$null}
    $taskMessage="$Label process exited"
    if($null -ne $taskCode){$taskMessage+=" (code $taskCode)"}
    if($taskDetail){$taskMessage+=": $taskDetail"}
    return $taskMessage
}
try{
    Save-TaskState
    $taskPython=Join-Path $taskRoot 'runtime/python/python.exe'
    if(-not (Test-Path -LiteralPath $taskPython)){$taskPython=(Get-Command python.exe).Source}
    foreach($taskDevice in $taskDevices){
        if(Test-Path -LiteralPath $taskDevice.Status){Remove-Item -LiteralPath $taskDevice.Status}
        $taskArgs='"'+(Join-Path $taskRoot 'src\usbip_reference_server.py')+'" --profile "'+$taskDevice.Profile+'" --port '+$taskDevice.UsbPort+' --radio-port '+$taskDevice.RadioPort+' --radio-mac '+$taskDevice.Mac+' --seconds '+$Seconds+' --status-file "'+$taskDevice.Status+'"'
        if($AudioProbe -and $taskDevice.Audio){$taskArgs+=' --audio-probe'}
        if($AudioBridge -and $taskDevice.Audio){$taskArgs+=' --audio-bridge'}
        $taskServer=Start-Process -FilePath $taskPython -ArgumentList $taskArgs -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput $taskDevice.ServerLog -RedirectStandardError $taskDevice.ServerError -PassThru
        if($AudioBridge -and $taskDevice.Audio){Set-TaskMediaPriority $taskServer}
        $taskServers+=,$taskServer
        $taskDevice.Server=@{Pid=$taskServer.Id;StartTicks=$taskServer.StartTime.ToUniversalTime().Ticks};Save-TaskState
        $taskReadyDeadline=(Get-Date).AddSeconds(5)
        while(-not (Test-Path -LiteralPath $taskDevice.Status)){
            if($taskServer.HasExited -or (Get-Date) -gt $taskReadyDeadline){throw "Local USB GIP server $($taskDevice.Name) did not become ready."}
            Start-Sleep -Milliseconds 100
        }
    }
    $taskArgs='run --instance "'+$taskAdapterId+'" --output usb-gip --usb-gip-mac '+$taskDevices[0].Mac+' --usb-gip-port '+$taskDevices[0].RadioPort+' --usb-gip-fallback '+$FallbackOutput+' --status-file "'+$taskControllerStatus+'" --firmware "'+(Join-Path $taskRoot 'work\firmware\FW_ACC_00U.bin')+'" --seconds '+$Seconds+' --stop-file "'+$taskStopPath+'"'
    foreach($taskDevice in $taskDevices | Select-Object -Skip 1){$taskArgs+=' --usb-gip-peer '+$taskDevice.Mac+':'+$taskDevice.RadioPort}
    $taskRadio=Start-Process -FilePath (Join-Path $taskRoot 'outputs\xone-probe.exe') -ArgumentList $taskArgs -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput $taskState.RadioLog -RedirectStandardError $taskState.RadioError -PassThru
    if($AudioBridge){Set-TaskMediaPriority $taskRadio}
    $taskState.Radio=@{Pid=$taskRadio.Id;StartTicks=$taskRadio.StartTime.ToUniversalTime().Ticks};Save-TaskState
    $taskDeadline=if($Seconds -eq 0){[datetime]::MaxValue}else{(Get-Date).AddSeconds($Seconds-2)}
    while((Get-Date) -lt $taskDeadline -and -not (Test-Path -LiteralPath $taskStopPath)){
        $taskRadio.Refresh()
        if($taskRadio.HasExited){throw (Get-TaskExitError $taskRadio 'Radio' $taskState.RadioError)}
        for($taskIndex=0;$taskIndex -lt $taskDevices.Count;++$taskIndex){
            $taskDevice=$taskDevices[$taskIndex];$taskServer=$taskServers[$taskIndex];$taskServer.Refresh()
            if($taskServer.HasExited){throw (Get-TaskExitError $taskServer ('USB server '+$taskDevice.Name) $taskDevice.ServerError)}
            $taskCurrent=Read-TaskRadioState $taskDevice.Status
            if(-not $taskCurrent){continue}
            if($taskDevice.Port -and (-not $taskCurrent.Ready -or $taskCurrent.Generation -ne $taskDevice.Generation)){Stop-TaskImportedPort $taskDevice}
            if(-not $taskDevice.Port -and $taskCurrent.Ready){
                $taskAttachLog=Join-Path $taskRoot ('work\usb-gip-'+$taskDevice.Name+'-attach.log')
                $taskAttachError=Join-Path $taskRoot ('work\usb-gip-'+$taskDevice.Name+'-attach.err')
                $taskAttach=Start-Process -FilePath $taskUsbip -ArgumentList ('-t '+$taskDevice.UsbPort+' attach -r 127.0.0.1 -b 1-1 --once --terse') -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $taskAttachLog -RedirectStandardError $taskAttachError
                if($taskAttach.ExitCode -ne 0){Write-Output "USB attach $($taskDevice.Name) not ready; retrying.";Start-Sleep -Milliseconds 1000;continue}
                $taskPortText=(Get-Content -LiteralPath $taskAttachLog -Raw).Trim()
                if($taskPortText -notmatch '^\d{1,3}$' -or [int]$taskPortText -lt 1 -or [int]$taskPortText -gt 255){throw 'Cannot identify the imported diagnostic port.'}
                $taskDevice.Port=[int]$taskPortText;$taskDevice.Generation=$taskCurrent.Generation;Save-TaskState
                Write-Output "Native USB GIP attached: device=$($taskDevice.Name) port=$($taskDevice.Port) generation=$($taskDevice.Generation)"
            }
        }
        Start-Sleep -Milliseconds 100
    }
}catch{$taskState.Error=$_.Exception.Message;Write-Output $taskState.Error}
finally{
    # Detach only ports allocated by this worker, never other USBip devices.
    foreach($taskDevice in $taskDevices){try{Stop-TaskImportedPort $taskDevice}catch{Write-Output $_.Exception.Message}}
    if($taskRadio -and -not $taskRadio.HasExited){
        Set-Content -LiteralPath $taskStopPath -Value stop
        if(-not $taskRadio.WaitForExit(5000)){$taskState.Error='Radio cleanup timed out; inspect logs before another run.'}
    }
    for($taskIndex=0;$taskIndex -lt $taskServers.Count;++$taskIndex){
        if(-not $taskServers[$taskIndex].HasExited -and (Test-TaskProcess $taskDevices[$taskIndex].Server)){Stop-Process -Id $taskServers[$taskIndex].Id}
    }
    $taskState.Running=$false;Save-TaskState
}
