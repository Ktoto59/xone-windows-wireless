# SPDX-License-Identifier: GPL-2.0-or-later
# Shared by the first-run UI and the uninstall guard. No action on dot-source.
. (Join-Path $PSScriptRoot 'Device-Config.ps1')
$script:XoneFirmwareHash='48084d9fa53b9bb04358f3bb127b7495dc8f7bb0b3ca1437bd24ef2b6eabdf66'
$script:XoneUsbipHash='38cad6d4432b52d5bb9409d9ad03b72fdffc4ada4cd3a48fbeca1a2752a8518a'
function Read-XoneJson([string]$Path){
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
        & (Join-Path $Root 'scripts/Run-UsbGip.ps1') -Stop
        $deadline=(Get-Date).AddSeconds(20)
        while((Test-XoneProcess $state.Worker) -and (Get-Date) -lt $deadline){Start-Sleep -Milliseconds 100}
        if(Test-XoneProcess $state.Worker){throw 'The bridge did not stop. Close it before continuing.'}
    }
    if($state -and (Test-XoneProcess $state.Radio)){throw 'The radio process is still running. Inspect the private work logs before continuing.'}
}
function Assert-XoneRadioIdle {
    $running=@(Get-CimInstance Win32_Process -Filter "Name = 'xone-probe.exe' OR Name = 'xone-probe-cpp.exe'" | Where-Object {$_.CommandLine -match '(?i)\b(run|controller-test|radio-test)\b'})
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
    $adapters=@(Get-PnpDevice -PresentOnly | Where-Object {$_.InstanceId -match '^USB\\VID_045E&PID_02FE\\[^\\]+$'} | ForEach-Object {
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
function Initialize-XonePrerequisites([string]$Root){
    if(-not (Test-Path -LiteralPath (Join-Path $Root 'runtime/python/python.exe'))){throw 'Bundled Python is missing. Reinstall the application.'}
    if(-not (Test-XoneFirmware $Root)){
        try{& (Join-Path $Root 'scripts/Import-Firmware.ps1')}catch{
            $downloads=Join-Path $Root 'work/downloads'
            $cab=Join-Path $downloads 'adapter-driver.cab'
            Get-XoneDownload 'https://download.windowsupdate.com/c/msdownload/update/driver/drvs/2017/07/1cd6a87c-623f-4407-a52d-c31be49e925c_e19f60808bdcbfbd3c3df6be3e71ffc52e43261e.cab' $cab
            & "$env:windir\System32\expand.exe" '-F:FW_ACC_00U.bin' $cab $downloads
            if($LASTEXITCODE -ne 0){throw 'Cannot extract the Microsoft firmware CAB.'}
            & (Join-Path $Root 'scripts/Import-Firmware.ps1') -Path (Join-Path $downloads 'FW_ACC_00U.bin')
        }
    }
    $usbip=Get-XoneUsbipStatus
    if(Test-Path -LiteralPath $usbip.Path){
        if(-not $usbip.Ready){throw 'An existing USBip installation has no ready virtual controller. Restart Windows or repair USBip; this wizard will not replace an existing installation.'}
        if($usbip.Version -notmatch '^0\.9\.8\.1(?:\D|$)'){throw ('Existing USBip '+$usbip.Version+' is not the tested 0.9.8.1. Use the tested upstream version; this wizard will not downgrade it.')}
        return
    }
    $installer=Join-Path $Root 'work/downloads/USBip-0.9.8.1-x64.exe'
    Get-XoneDownload 'https://github.com/vadimgrn/usbip-win2/releases/download/v.0.9.8.1/USBip-0.9.8.1-x64.exe' $installer $script:XoneUsbipHash
    $signature=Get-AuthenticodeSignature -LiteralPath $installer
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Scheibling Consulting AB'){throw 'USBip publisher/signature verification failed.'}
    $log=Join-Path $Root 'work/usbip-setup.log'
    $arguments='/VERYSILENT /SP- /SUPPRESSMSGBOXES /NORESTART /RESTARTEXITCODE=3010 /NOCLOSEAPPLICATIONS /NORESTARTAPPLICATIONS /COMPONENTS="main,client" /TASKS="" /LOG='+ (Quote-XonePath $log)
    $process=Start-Process -FilePath $installer -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru -Wait
    if($process.ExitCode -eq 3010){throw 'USBip installed successfully and requires a Windows restart. Restart, then reopen this wizard.'}
    if($process.ExitCode -ne 0){throw ('USBip installation failed (code '+$process.ExitCode+'). See work/usbip-setup.log.')}
    if(-not (Get-XoneUsbipStatus).Ready){throw 'USBip installed but its virtual controller is not ready. Restart Windows and reopen this wizard.'}
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
    [void](Read-XoneDeviceConfig $path)
    $value=Read-XoneJson $path
    if(-not $value){throw 'Save the device configuration before choosing a bridge.'}
    $value | Add-Member -NotePropertyName BridgeBackend -NotePropertyValue $Backend -Force
    Write-XoneJson $path $value
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
    if($service -ne 'mt7612US'){throw 'Stock driver restoration is not verified. Keep this installation and its recovery data.'}
}
function Assert-XonePanelClosed([string]$Root){
    $path=Join-Path $Root 'work/control-panel.lock'
    if(-not (Test-Path -LiteralPath $path)){return}
    $stream=$null
    try{$stream=[IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)}
    catch [IO.IOException]{throw 'Close the xone-windows-wireless control panel before updating or uninstalling.'}
    finally{if($stream){$stream.Dispose()}}
}
