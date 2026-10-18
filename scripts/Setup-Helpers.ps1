# SPDX-License-Identifier: GPL-2.0-or-later
# Shared by the first-run UI and the uninstall guard. No action on dot-source.
. (Join-Path $PSScriptRoot 'Device-Config.ps1')
$script:XoneFirmwareHash='48084d9fa53b9bb04358f3bb127b7495dc8f7bb0b3ca1437bd24ef2b6eabdf66'
$script:XoneFirmwareCabHash='65736a84ff4036645b8f8ec602bed91ab6353019c9cb3233decab9feec0f6f04'
$script:XoneUsbipHash='38cad6d4432b52d5bb9409d9ad03b72fdffc4ada4cd3a48fbeca1a2752a8518a'
function Get-XoneMemoryHost([string]$Root){
    foreach($relative in @('outputs','outputs/appliance-candidate')){
        $folder=Join-Path $Root $relative
        if([IO.File]::Exists((Join-Path $folder 'runtime-capabilities.json')) -and [IO.File]::Exists((Join-Path $folder 'xone-background.exe'))){return (Join-Path $folder 'xone-background.exe')}
    }
    return $null
}
function Get-XoneRuntimeState([string]$Root){
    $hostPath=Get-XoneMemoryHost $Root
    if(-not $hostPath){return $null}
    $text=(& $hostPath --status | Out-String)
    if($LASTEXITCODE -ne 0){throw 'Cannot query the in-memory runtime state.'}
    if($text.Trim()) {return ($text | ConvertFrom-Json)}
    return $null
}
function Read-XoneJson([string]$Path){
    $name=[IO.Path]::GetFileName($Path)
    if($name -in @('maintenance-status.json','usb-gip-running.json','usb-gip-controller-status.json') -or $name -match '^native-slot-(\d+)\.json$'){
        $root=Split-Path -Parent (Split-Path -Parent $Path)
        if(Get-XoneMemoryHost $root){
            $state=Get-XoneRuntimeState $root
            if($name -eq 'maintenance-status.json'){return $state.Host}
            if($name -eq 'usb-gip-running.json'){return $state.Runtime}
            if($name -eq 'usb-gip-controller-status.json'){return $state.Radio}
            if($name -match '^native-slot-(\d+)\.json$'){
                $slot=[int]$Matches[1];if($state.Slots -and $slot -lt $state.Slots.Count){return $state.Slots[$slot]};return $null
            }
        }
    }
    $stream=$null;$reader=$null
    try{
        $stream=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8)
        return ($reader.ReadToEnd() | ConvertFrom-Json)
    }catch [IO.IOException]{return $null}
    finally{if($reader){$reader.Dispose()}elseif($stream){$stream.Dispose()}}
}
function Write-XoneJson([string]$Path,$Value){
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    $temporary=$Path+'.tmp'
    [IO.File]::WriteAllText($temporary,($Value | ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
    if([IO.File]::Exists($Path)){[IO.File]::Replace($temporary,$Path,[NullString]::Value)}else{[IO.File]::Move($temporary,$Path)}
}
function Quote-XonePath([string]$Value){
    if($Value -match '["\r\n]' -or $Value.EndsWith('\')){throw 'Unsupported command argument.'}
    return '"'+$Value+'"'
}
function Test-XoneProcess($Identity){
    if(-not $Identity){return $false}
    $process=Get-Process -Id $Identity.Pid -ErrorAction SilentlyContinue
    return [bool]($process -and $process.StartTime.ToUniversalTime().Ticks -eq [long]$Identity.StartTicks)
}
function Stop-XoneBridge([string]$Root){
    $path=Join-Path $Root 'work/usb-gip-running.json'
    $state=Read-XoneJson $path
    if($state -and (Test-XoneProcess $state.Worker)){
        $memoryHost=Get-XoneMemoryHost $Root
        if($memoryHost){& $memoryHost --stop | Out-Null}else{
            [IO.File]::WriteAllText((Join-Path $Root 'work/usb-gip.stop'),'stop');[IO.File]::WriteAllText((Join-Path $Root 'work/maintenance.stop'),'stop')
        }
        $deadline=(Get-Date).AddSeconds(20)
        while((Test-XoneProcess $state.Worker) -and (Get-Date) -lt $deadline){Start-Sleep -Milliseconds 100}
        if(Test-XoneProcess $state.Worker){throw 'The bridge did not stop. Close it before continuing.'}
    }
    if($state -and (Test-XoneProcess $state.Radio)){throw 'The radio process is still running. Inspect the private work logs before continuing.'}
}
function Assert-XoneRadioIdle {
    $running=@(Get-CimInstance Win32_Process -Filter "Name = 'xone-probe.exe' OR Name = 'xone-probe-cpp.exe' OR Name = 'xone-native.exe'" | Where-Object {$_.CommandLine -match '(?i)\b(run|controller-test|radio-test)\b'})
    if($running.Count){throw 'Another xone radio session is running. Stop that session before configuring this installation.'}
}
function Stop-XoneManagedBridge([string]$Root){
    $scriptPath=Join-Path $Root 'scripts/Run-MaintainedBridge.ps1'
    if(Test-Path -LiteralPath $scriptPath){& $scriptPath -Stop}else{Stop-XoneBridge $Root}
}
function Test-XoneFirmware([string]$Root){
    $path=Join-Path $Root 'work/firmware/FW_ACC_00U.bin'
    return [bool]((Test-Path -LiteralPath $path) -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $script:XoneFirmwareHash)
}
function Get-XoneUsbipStatus {
    $path=Join-Path $env:ProgramW6432 'USBip/usbip.exe'
    $ready=$false;$version=''
    if(Test-Path -LiteralPath $path){
        $version=(Get-Item -LiteralPath $path).VersionInfo.FileVersion
        $devices=@(Get-PnpDevice -PresentOnly | Where-Object {$_.InstanceId -like 'ROOT\USB\*'})
        foreach($device in $devices){
            $service=(Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
            $problem=(Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_ProblemCode -ErrorAction SilentlyContinue).Data
            if($service -eq 'usbip2_ude' -and $problem -eq 0){$ready=$true}
        }
    }
    return [pscustomobject]@{Path=$path;Version=$version;Ready=$ready}
}
function Get-XoneSetupStatus([string]$Root){
    $adapters=@(Get-PnpDevice -PresentOnly | Where-Object {Test-XoneAdapterInstance $_.InstanceId} | ForEach-Object {
        [pscustomobject]@{InstanceId=$_.InstanceId;Name=$_.FriendlyName;Service=(Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Service).Data}
    })
    return [pscustomobject]@{Firmware=(Test-XoneFirmware $Root);Usbip=(Get-XoneUsbipStatus);Adapters=$adapters;Python=(Test-Path -LiteralPath (Join-Path $Root 'runtime/python/python.exe'))}
}
function Get-XoneDownload([string]$Url,[string]$Path,[string]$Hash=''){
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    if(Test-Path -LiteralPath $Path){
        if($Hash -and (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Hash){return}
        Remove-Item -LiteralPath $Path
    }
    [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
    $temporary=$Path+'.partial'
    try{
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $temporary -TimeoutSec 180
        if($Hash -and (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Hash){throw 'Downloaded file checksum does not match the pinned release.'}
        Move-Item -LiteralPath $temporary -Destination $Path
    }finally{if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary}}
}
function Invoke-XonePnputil([string[]]$Arguments){
    & (Get-XoneSystemTool 'pnputil.exe') @Arguments | Out-Host
    return $LASTEXITCODE
}
function Get-XoneDeviceService([string]$InstanceId){
    return [string](Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
}
function Initialize-XoneStockDriver([string]$Root,[string]$InstanceId){
    $package=Get-XoneAdapterPackage $InstanceId
    $script:XoneSetupStage='StockDriverCheck'
    $service=Get-XoneDeviceService $InstanceId
    if($service -eq 'WinUSB' -or $service -eq $package.Service){return $service}
    if($service){throw 'The adapter is owned by an unexpected driver. Setup will not replace another application driver.'}
    $script:XoneSetupStage='StockDriverDownload'
    $downloads=Join-Path $Root 'work/downloads'
    # Reuse the same verified CAB when firmware preparation downloaded it.
    $cabName=if($package.Model -eq '02FE'){'adapter-driver.cab'}else{'adapter-'+$package.Model+'.cab'}
    $cab=Join-Path $downloads $cabName
    Get-XoneDownload $package.Url $cab $package.Hash
    $directory=Join-Path $downloads ('stock-'+$package.Model)
    [void][IO.Directory]::CreateDirectory($directory)
    $script:XoneSetupStage='StockDriverExtraction'
    & (Get-XoneSystemTool 'expand.exe') '-F:*' $cab $directory | Out-Host
    if($LASTEXITCODE -ne 0){throw 'Cannot extract the Microsoft adapter driver package.'}
    $script:XoneSetupStage='StockDriverSignatureCheck'
    $signature=Get-AuthenticodeSignature -LiteralPath (Join-Path $directory $package.Catalog)
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=Microsoft Windows Hardware Compatibility Publisher,'){throw 'Microsoft adapter driver catalog verification failed.'}
    $inf=Join-Path $directory $package.Inf
    $hardwareId='USB\VID_045E&PID_'+$package.Model
    if(-not [IO.File]::ReadAllText($inf).Contains($hardwareId)){throw 'The verified driver package does not match the selected adapter model.'}
    $script:XoneSetupStage='StockDriverInstallation'
    # Windows validates catalog membership/signatures before accepting the INF.
    # Only one supported adapter is selected, and this INF matches one model.
    $code=Invoke-XonePnputil @('/add-driver',$inf,'/install')
    if($code -notin @(0,3010)){throw ('Microsoft adapter driver installation failed (code '+$code+').')}
    if($code -eq 3010){Write-XoneJson (Join-Path $Root 'work/setup-restart-required.json') @{Required=$true;Reason='Microsoft adapter driver installation'}}
    $deadline=[datetime]::UtcNow.AddSeconds(30)
    do{
        $service=Get-XoneDeviceService $InstanceId
        if($service -eq $package.Service){return $service}
        Start-Sleep -Milliseconds 250
    }while([datetime]::UtcNow -lt $deadline)
    throw 'Windows did not bind the installed Microsoft adapter driver. Setup has retained its installation log.'
}
function Initialize-XonePrerequisites([string]$Root,[switch]$AllowPendingRestart){
    $script:XoneSetupStage='RuntimeCheck'
    if(-not (Test-Path -LiteralPath (Join-Path $Root 'outputs/xone-probe-cpp.exe')) -and -not (Test-Path -LiteralPath (Join-Path $Root 'runtime/python/python.exe'))){throw 'The bridge runtime is missing. Reinstall the application.'}
    if(-not (Test-XoneFirmware $Root)){
        $script:XoneSetupStage='FirmwarePreparation'
        try{& (Join-Path $Root 'scripts/Import-Firmware.ps1')}catch{
            $downloads=Join-Path $Root 'work/downloads'
            $cab=Join-Path $downloads 'adapter-driver.cab'
            $script:XoneSetupStage='FirmwareDownload'
            Get-XoneDownload 'https://catalog.s.download.windowsupdate.com/c/msdownload/update/driver/drvs/2017/07/1cd6a87c-623f-4407-a52d-c31be49e925c_e19f60808bdcbfbd3c3df6be3e71ffc52e43261e.cab' $cab $script:XoneFirmwareCabHash
            $script:XoneSetupStage='FirmwareExtraction'
            & (Get-XoneSystemTool 'expand.exe') '-F:FW_ACC_00U.bin' $cab $downloads
            if($LASTEXITCODE -ne 0){throw 'Cannot extract the Microsoft firmware CAB.'}
            & (Join-Path $Root 'scripts/Import-Firmware.ps1') -Path (Join-Path $downloads 'FW_ACC_00U.bin')
        }
    }
    $script:XoneSetupStage='UsbipCheck'
    $usbip=Get-XoneUsbipStatus
    if(Test-Path -LiteralPath $usbip.Path){
        if($usbip.Version -notmatch '^0\.9\.8\.1(?:\D|$)'){throw ('Existing USBip '+$usbip.Version+' is not the tested 0.9.8.1. Setup will not downgrade another installed version.')}
        if($usbip.Ready){return}
        # Repair an incomplete installation of the exact tested dependency.
        # This also lets a failed first setup be retried using this installer.
    }
    $installer=Join-Path $Root 'work/downloads/USBip-0.9.8.1-x64.exe'
    $script:XoneSetupStage='UsbipDownload'
    Get-XoneDownload 'https://github.com/vadimgrn/usbip-win2/releases/download/v.0.9.8.1/USBip-0.9.8.1-x64.exe' $installer $script:XoneUsbipHash
    $script:XoneSetupStage='UsbipSignatureCheck'
    $signature=Get-AuthenticodeSignature -LiteralPath $installer
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Scheibling Consulting AB'){throw 'USBip publisher/signature verification failed.'}
    $arguments='/VERYSILENT /SP- /SUPPRESSMSGBOXES /NORESTART /RESTARTEXITCODE=3010 /NOCLOSEAPPLICATIONS /NORESTARTAPPLICATIONS /COMPONENTS="main,client" /TASKS=""'
    $script:XoneSetupStage='UsbipInstallation'
    $process=Start-Process -FilePath $installer -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru
    [void]$process.Handle;$process.WaitForExit()
    if($process.ExitCode -eq 3010){
        Write-XoneJson (Join-Path $Root 'work/setup-restart-required.json') @{Required=$true;Reason='USBip driver installation'}
        if($AllowPendingRestart){return}
        throw 'USBip installed successfully and requires a Windows restart.'
    }
    if($process.ExitCode -ne 0){throw ('USBip installation failed (code '+$process.ExitCode+').')}
    $deadline=[datetime]::UtcNow.AddSeconds(15)
    do{if((Get-XoneUsbipStatus).Ready){return};Start-Sleep -Milliseconds 250}while([datetime]::UtcNow -lt $deadline)
    throw 'USBip installation finished but Windows did not start its virtual controller.'
}
function Set-XoneSetupConfig([string]$Root,[string]$AdapterId,$Entries){
    $path=Join-Path $Root 'config/devices.local.json'
    $temporary=Join-Path $Root ('work/config-check-'+[guid]::NewGuid().ToString('N')+'.json')
    $value=[ordered]@{Version=1;AdapterInstanceId=$AdapterId;Devices=@($Entries)}
    $previous=Read-XoneJson $path
    if($previous -and $previous.BridgeBackend){$value.BridgeBackend=[string]$previous.BridgeBackend}
    try{Write-XoneJson $temporary $value;[void](Read-XoneDeviceConfig $temporary);Write-XoneJson $path $value}
    finally{if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary}}
}
function Get-XoneDiscoveredMacs([string]$Text){
    $seen=@{}
    foreach($match in [regex]::Matches($Text,'(?im)^ASSOCIATED WCID=\d+ MAC=([0-9a-f]{12})\s*$')){
        $mac=$match.Groups[1].Value.ToLowerInvariant()
        if(-not $seen.ContainsKey($mac)){$seen[$mac]=$true;Write-Output $mac}
    }
}
function Set-XoneBridgeBackend([string]$Root,[ValidateSet('cpp','python')][string]$Backend){
    if($Backend -eq 'cpp' -and -not [IO.File]::Exists((Join-Path $Root 'outputs/xone-probe-cpp.exe'))){throw 'This installation does not include the C++ bridge.'}
    $path=Join-Path $Root 'config/devices.local.json'
    [void](Read-XoneDeviceConfig $path -AllowEmpty)
    $value=Read-XoneJson $path
    if(-not $value){throw 'Save the device configuration before choosing a bridge.'}
    $value | Add-Member -NotePropertyName BridgeBackend -NotePropertyValue $Backend -Force
    Write-XoneJson $path $value
}
function Get-XoneRecoveryInf($State){return (Join-Path (Join-Path $env:windir 'INF') $State.InfName)}
function Assert-XoneRecovery([string]$Root,[string]$InstanceId){
    $saved=Read-XoneJson (Join-Path $Root 'work/binding-state.json')
    if(-not (Test-XoneStockBinding $saved) -or $saved.InstanceId -ne $InstanceId){
        throw 'Driver recovery data is missing or belongs to another adapter. Keep the previous installation and transfer its matching binding-state.json and driver-backup before retrying Setup.'
    }
    foreach($name in @('UpperFilters','InterfaceGuids')){
        if($saved.$name.Present -isnot [bool] -or -not $saved.$name.PSObject.Properties['Value']){throw 'Driver recovery registry snapshots are incomplete. Keep the original backup and retry Setup.'}
    }
    $backupRoot=Join-Path $Root 'work/driver-backup'
    if(-not [IO.Directory]::Exists($backupRoot)){throw 'The driver-backup folder is missing. Keep the previous installation and transfer its recovery backup before retrying Setup.'}
    # OneDrive placeholders also have ReparsePoint; only actual filesystem links
    # redirect a backup outside its folder. Read/hash operations hydrate cloud files.
    if((Get-Item -LiteralPath $backupRoot -Force).LinkType -in @('SymbolicLink','Junction')){throw 'A linked driver-backup folder is not accepted.'}
    $items=@(Get-ChildItem -LiteralPath $backupRoot -Recurse -Force)
    if(@($items | Where-Object {$_.LinkType -in @('SymbolicLink','Junction')}).Count){throw 'Links are not accepted inside driver recovery backups.'}
    $package=Get-XoneAdapterPackage $InstanceId
    if(-not @($items | Where-Object {-not $_.PSIsContainer -and $_.Name -eq $package.Catalog}).Count -or
       -not @($items | Where-Object {-not $_.PSIsContainer -and $_.Extension -eq '.sys'}).Count){throw 'The saved driver catalog or driver binary is missing. Keep the original driver backup and retry Setup.'}
    $infs=@($items | Where-Object {-not $_.PSIsContainer -and $_.Name -eq $package.Inf})
    $installedInf=Get-XoneRecoveryInf $saved
    if($infs.Count -ne 1 -or -not [IO.File]::Exists($installedInf) -or
       (Get-FileHash -LiteralPath $infs[0].FullName -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $installedInf -Algorithm SHA256).Hash){
        throw 'The saved Microsoft driver package could not be verified against its installed INF. Keep the recovery backup and repair it before retrying Setup.'
    }
    return $saved
}
function Copy-XoneRecovery([string]$OldRoot,[string]$Root,[string]$InstanceId){
    $target=Join-Path $Root 'work/binding-state.json'
    if([IO.File]::Exists($target)){[void](Assert-XoneRecovery $Root $InstanceId);return}
    $saved=Assert-XoneRecovery $OldRoot $InstanceId
    $source=[IO.Path]::GetFullPath((Join-Path $OldRoot 'work/driver-backup')).TrimEnd('\')+'\'
    $destination=Join-Path $Root 'work/driver-backup'
    if([IO.Directory]::Exists($destination)){
        $existing=@(Get-Item -LiteralPath $destination -Force)+@(Get-ChildItem -LiteralPath $destination -Recurse -Force)
        if(@($existing | Where-Object {$_.LinkType -in @('SymbolicLink','Junction')}).Count){throw 'Links are not accepted inside the destination recovery backup.'}
    }
    foreach($file in Get-ChildItem -LiteralPath $source -Recurse -File -Force){
        if(-not $file.FullName.StartsWith($source,[StringComparison]::OrdinalIgnoreCase)){throw 'Recovery file outside the backup folder refused.'}
        $newPath=Join-Path $destination $file.FullName.Substring($source.Length)
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $newPath))
        if(-not [IO.File]::Exists($newPath)){[IO.File]::Copy($file.FullName,$newPath,$false)}
        if((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $newPath -Algorithm SHA256).Hash){throw 'Recovery copy differs from an existing backup; no metadata was replaced.'}
    }
    # Publish the registry/binding snapshots only after every backup file is safe.
    Write-XoneJson $target $saved
    [void](Assert-XoneRecovery $Root $InstanceId)
}
function Initialize-XoneApplication([string]$Root,[string]$InstanceId,[switch]$AllowPendingRestart){
    $restartMarker=Join-Path $Root 'work/setup-restart-required.json'
    if([IO.File]::Exists($restartMarker)){
        # A completed Windows reboot must not force every later upgrade to reboot.
        $boot=(Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime.ToUniversalTime()
        if([IO.File]::GetLastWriteTimeUtc($restartMarker) -lt $boot){[IO.File]::Delete($restartMarker)}
    }
    $script:XoneSetupStage='AdapterDetection'
    $InstanceId=Resolve-XoneAdapterInstance $InstanceId
    $path=Join-Path $Root 'config/devices.local.json'
    $config=Read-XoneJson $path
    if($config){
        [void](Read-XoneDeviceConfig $path -AllowEmpty)
        if($config.AdapterInstanceId -and $config.AdapterInstanceId -ne $InstanceId){throw 'This configuration belongs to a different adapter. Select it or use Advanced settings.'}
    }
    $script:XoneSetupStage='RecoveryValidation'
    if((Get-XoneDeviceService $InstanceId) -eq 'WinUSB'){[void](Assert-XoneRecovery $Root $InstanceId)}
    $script:XoneSetupStage='StopExistingBridge'
    Stop-XoneManagedBridge $Root;Assert-XoneRadioIdle
    Initialize-XonePrerequisites $Root -AllowPendingRestart:$AllowPendingRestart
    $service=Initialize-XoneStockDriver $Root $InstanceId
    $package=Get-XoneAdapterPackage $InstanceId
    $switched=$false
    if($service -eq $package.Service){
        $script:XoneSetupStage='WinusbBinding'
        & (Join-Path $Root 'scripts/Switch-WinUSB.ps1') -Mode Bind -InstanceId $InstanceId
        $switched=$true
    }elseif($service -ne 'WinUSB'){throw 'Adapter preparation did not select the expected driver.'}
    $script:XoneSetupStage='RecoveryValidation'
    [void](Assert-XoneRecovery $Root $InstanceId)
    $reconnect=$false
    if($switched){
        $script:XoneSetupStage='AdapterRestart'
        $restartCode=Invoke-XonePnputil @('/restart-device',$InstanceId)
        $reconnect=$restartCode -notin @(0,3010)
        if($restartCode -eq 3010){Write-XoneJson (Join-Path $Root 'work/setup-restart-required.json') @{Required=$true;Reason='Adapter driver installation'};if(-not $AllowPendingRestart){throw 'Windows requires a restart to finish adapter setup.'}}
    }
    $script:XoneSetupStage='SaveAdapterConfiguration'
    if(-not $config){$config=[pscustomobject]@{Version=1;AdapterInstanceId=$InstanceId;BridgeBackend='cpp';Devices=@()}}
    $config.AdapterInstanceId=$InstanceId
    if(-not $config.BridgeBackend){$config | Add-Member -NotePropertyName BridgeBackend -NotePropertyValue cpp -Force}
    Write-XoneJson $path $config
    Write-XoneJson (Join-Path $Root 'work/application-setup.json') @{Complete=$true;ReconnectRequired=$reconnect;UpdatedUtc=[datetime]::UtcNow.ToString('o')}
    return [pscustomobject]@{ReconnectRequired=$reconnect}
}
function Move-XonePreviousInstallation([string]$Root){
    $preferences=Read-XoneJson (Join-Path $Root 'config/app.local.json')
    $sid=if($preferences.OwnerSid){[string]$preferences.OwnerSid}else{[Security.Principal.WindowsIdentity]::GetCurrent().User.Value}
    if($sid -notmatch '^S-1-5-21-\d+-\d+-\d+-\d+$'){throw 'Invalid installation owner.'}
    $task=Get-ScheduledTask -TaskName ('xone-windows-wireless-'+$sid) -ErrorAction SilentlyContinue
    if(-not $task){return}
    $actions=@($task.Actions)
    if($actions.Count -ne 1 -or $actions[0].Arguments -or [IO.Path]::GetFileName($actions[0].Execute) -ne 'xone-background.exe'){throw 'The existing startup task is not an owned xone background host.'}
    $oldRoot=Split-Path -Parent (Split-Path -Parent $actions[0].Execute)
    if([IO.Path]::GetFullPath($oldRoot) -eq [IO.Path]::GetFullPath($Root)){return}
    Assert-XonePanelClosed $oldRoot
    $oldConfig=Join-Path $oldRoot 'config/devices.local.json';$newConfig=Join-Path $Root 'config/devices.local.json'
    $previous=Read-XoneDeviceConfig $oldConfig -AllowEmpty
    $current=Read-XoneJson $newConfig
    if(-not $previous.AdapterInstanceId -or ($current.AdapterInstanceId -and $current.AdapterInstanceId -ne $previous.AdapterInstanceId)){throw 'Previous and current installations do not select the same adapter.'}
    # Finish copying recovery before unregistering the only pointer to OldRoot.
    # A failed copy leaves the old task and all original recovery files in place.
    Copy-XoneRecovery $oldRoot $Root $previous.AdapterInstanceId
    if(-not [IO.File]::Exists($newConfig)){
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $newConfig))
        Copy-Item -LiteralPath $oldConfig -Destination $newConfig
    }
    # Never execute scripts/commands from the old installation with setup rights.
    $oldMemoryHost=Get-XoneMemoryHost $oldRoot
    if($oldMemoryHost){& $oldMemoryHost --stop | Out-Null}else{
        foreach($name in @('maintenance.stop','usb-gip.stop')){[IO.File]::WriteAllText((Join-Path $oldRoot ('work/'+$name)),'stop')}
    }
    $state=Read-XoneJson (Join-Path $oldRoot 'work/maintenance-status.json');$deadline=[datetime]::UtcNow.AddSeconds(35)
    while($state -and (Test-XoneProcess $state.Process) -and [datetime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 200}
    if($state -and (Test-XoneProcess $state.Process)){throw 'The previous installation did not stop; its recovery data is intact.'}
    Stop-ScheduledTask -TaskName $task.TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $task.TaskName -Confirm:$false
}
function Add-XoneApplicationDevice([string]$Root,[string]$InstanceId,[ValidateSet('controller-0b12-usb.json','headset-0b17-usb.json','headset-0b32-usb.json')][string]$Profile){
    $path=Join-Path $Root 'config/devices.local.json'
    $config=Read-XoneDeviceConfig $path -AllowEmpty
    $InstanceId=Resolve-XoneAdapterInstance $InstanceId
    if($config.AdapterInstanceId -and $config.AdapterInstanceId -ne $InstanceId){throw 'Select the adapter saved by this application.'}
    Stop-XoneManagedBridge $Root;Assert-XoneRadioIdle
    if(-not (Test-XoneFirmware $Root)){throw 'Complete adapter setup first.'}
    if((Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_Service).Data -ne 'WinUSB'){throw 'Complete adapter setup first.'}
    $stamp=[guid]::NewGuid().ToString('N');$stop=Join-Path $Root ('work/enroll-'+$stamp+'.stop');$log=Join-Path $Root ('work/enroll-'+$stamp+'.log');$errorLog=Join-Path $Root ('work/enroll-'+$stamp+'.err')
    $arguments='controller-test --instance '+(Quote-XonePath $InstanceId)+' --firmware '+(Quote-XonePath (Join-Path $Root 'work/firmware/FW_ACC_00U.bin'))+' --seconds 90 --stop-file '+(Quote-XonePath $stop)
    $process=Start-Process -FilePath (Join-Path $Root 'outputs/xone-probe.exe') -ArgumentList $arguments -WorkingDirectory $Root -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError $errorLog -PassThru
    $found=$null;$stream=$null;$reader=$null
    try{
        $deadline=[datetime]::UtcNow.AddSeconds(90);$observed=@{};$pendingText=''
        while(-not $process.HasExited -and [datetime]::UtcNow -lt $deadline){
            if(-not $reader -and [IO.File]::Exists($log)){$stream=[IO.FileStream]::new($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete));$reader=[IO.StreamReader]::new($stream)}
            if($reader){
                $pendingText+=$reader.ReadToEnd()
                $lastNewline=$pendingText.LastIndexOf("`n")
                $completeText=if($lastNewline -ge 0){$pendingText.Substring(0,$lastNewline+1)}else{''}
                $pendingText=if($lastNewline -ge 0){$pendingText.Substring($lastNewline+1)}else{$pendingText}
                if($pendingText.Length -gt 131072){throw 'Malformed enrollment output. Restart the adapter and try again.'}
                foreach($line in ($completeText -split "`n")){
                    foreach($mac in @(Get-XoneDiscoveredMacs $line)){if($mac -notin @($config.Devices.Mac)){$observed[$mac]=$true}}
                }
            }
            if($observed.Count -gt 1){throw 'More than one new device was found. Turn off the other Xbox devices and try again.'}
            if($observed.Count -eq 1){$found=@($observed.Keys)[0];break}
            Start-Sleep -Milliseconds 250;$process.Refresh()
        }
        if(-not $found){throw 'No new device connected. Turn on pairing on the selected device and press the adapter pairing button, then try again.'}
    }finally{
        if($reader){$reader.Dispose()}elseif($stream){$stream.Dispose()}
        [IO.File]::WriteAllText($stop,'stop')
        if(-not $process.WaitForExit(10000)){$process.Kill();$process.WaitForExit();throw 'The enrollment radio session did not stop cleanly. Reconnect the adapter before retrying.'}
    }
    if($process.ExitCode -ne 0){throw 'The radio session failed. Reconnect the adapter and retry; your saved devices were preserved.'}
    $name=switch($Profile){'controller-0b12-usb.json'{'controller'};'headset-0b17-usb.json'{'older-headset'};'headset-0b32-usb.json'{'newer-headset'}}
    $original=Read-XoneJson $path
    $entries=@($original.Devices | Where-Object {$_.Profile -ne $Profile})+@([pscustomobject]@{Name=$name;Mac=$found;Profile=$Profile})
    Set-XoneSetupConfig $Root $InstanceId $entries
}
function Restore-XoneForRemoval([string]$Root){
    Assert-XonePanelClosed $Root
    Stop-XoneBridge $Root
    $state=Read-XoneJson (Join-Path $Root 'work/binding-state.json')
    if(-not $state){return}
    $device=Get-PnpDevice -InstanceId $state.InstanceId -PresentOnly -ErrorAction SilentlyContinue
    if(-not $device){throw 'Connect the adapter to its original USB port so its stock driver can be restored before uninstalling.'}
    $service=(Get-PnpDeviceProperty -InstanceId $state.InstanceId -KeyName DEVPKEY_Device_Service).Data
    if($service -eq 'WinUSB'){
        Assert-XoneRadioIdle
        & (Join-Path $Root 'scripts/Switch-WinUSB.ps1') -Mode Restore -InstanceId $state.InstanceId
        $service=(Get-PnpDeviceProperty -InstanceId $state.InstanceId -KeyName DEVPKEY_Device_Service).Data
    }
    if($service -ne (Get-XoneAdapterPackage $state.InstanceId).Service){throw 'Stock driver restoration is not verified. Keep this installation and its recovery data.'}
}
function Assert-XonePanelClosed([string]$Root){
    foreach($name in @('control-panel.lock','mode-switch.lock')){
        $path=Join-Path $Root ('work/'+$name)
        if(-not (Test-Path -LiteralPath $path)){continue}
        $stream=$null
        try{$stream=[IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)}
        catch [IO.IOException]{throw 'Close the xone-windows-wireless control panel or wait for Start/Stop to finish before updating or uninstalling.'}
        finally{if($stream){$stream.Dispose()}}
    }
}
