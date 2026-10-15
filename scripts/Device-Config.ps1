# SPDX-License-Identifier: GPL-2.0-or-later
function Test-XoneAdapterInstance([string]$InstanceId){
    return $InstanceId -match '^USB\\VID_045E&PID_(02FE|02E6|02F9|091E)\\[A-Z0-9_&-]+\z'
}
function Get-XoneAdapterPackage([string]$InstanceId){
    if(-not (Test-XoneAdapterInstance $InstanceId)){throw 'Unsupported Xbox Wireless Adapter instance.'}
    $model=$InstanceId.Substring(17,4).ToUpperInvariant()
    $base='https://catalog.s.download.windowsupdate.com/'
    switch($model){
        '02FE' {$service='mt7612US';$inf='mt7612us_RL.inf';$catalog='mt7612us.cat';$hash='65736a84ff4036645b8f8ec602bed91ab6353019c9cb3233decab9feec0f6f04';$url=$base+'c/msdownload/update/driver/drvs/2017/07/1cd6a87c-623f-4407-a52d-c31be49e925c_e19f60808bdcbfbd3c3df6be3e71ffc52e43261e.cab'}
        '02E6' {$service='mt7612US';$inf='mt7612us.inf';$catalog='mt7612us.cat';$hash='e6388a27a75756920de73af2468e3ad55d7afef59abc3a0b508094e091d19f08';$url=$base+'d/msdownload/update/driver/drvs/2015/12/20810869_8ce2975a7fbaa06bcfb0d8762a6275a1cf7c1dd3.cab'}
        '02F9' {$service='mt7612US_bc';$inf='mt7612us_CL.inf';$catalog='mt7612us_bc.cat';$hash='90dd91fb67460d387407fd8109c0e7d0f18f4b659be0f62db858a1849ead2ee0';$url=$base+'c/msdownload/update/driver/drvs/2017/06/1dbd7cb4-53bc-4857-a5b0-5955c8acaf71_9081931e7d664429a93ffda0db41b7545b7ac257.cab'}
        '091E' {$service='mt7612US_bc';$inf='mt7612us_BR.inf';$catalog='mt7612us_bc.cat';$hash='d3d660859611c734d71739590ff4cc5940e061b816a10c5eaa70187645183ccb';$url=$base+'c/msdownload/update/driver/drvs/2018/09/e5339a2a-0cbf-4100-ae09-81dab77d8ab2_56ceef39e5f673aa1ea7dd3c17e71b5bd2add2f7.cab'}
        default {throw 'Adapter package metadata is missing.'}
    }
    return [pscustomobject]@{Model=$model;Service=$service;Inf=$inf;Catalog=$catalog;Hash=$hash;Url=$url;Section='MTWLAN.ndi'}
}
function Test-XoneStockBinding($State){
    if(-not $State -or -not (Test-XoneAdapterInstance $State.InstanceId)){return $false}
    $package=Get-XoneAdapterPackage $State.InstanceId
    return $State.Service -eq $package.Service -and $State.InfName -match '^oem\d+\.inf$' -and $State.Section -eq $package.Section
}
function Resolve-XoneAdapterInstance([string]$InstanceId=''){
    if($InstanceId){
        if(-not (Test-XoneAdapterInstance $InstanceId)){throw 'Unsupported Xbox Wireless Adapter instance. Supported IDs: 045E:02FE, 02E6, 02F9, 091E.'}
        $device=Get-PnpDevice -InstanceId $InstanceId -PresentOnly -ErrorAction Stop
        if(-not $device){throw 'The selected adapter is not connected.'}
        return $InstanceId
    }
    $taskAdapters=@(Get-PnpDevice -PresentOnly | Where-Object {Test-XoneAdapterInstance $_.InstanceId})
    if(-not $taskAdapters.Count){throw 'No supported Xbox Wireless Adapter was found. Connect an adapter (045E:02FE, 02E6, 02F9 or 091E) and retry.'}
    if($taskAdapters.Count -gt 1){throw 'More than one Xbox Wireless Adapter is connected. Disconnect the extra adapter and retry.'}
    return $taskAdapters[0].InstanceId
}
function Read-XoneDeviceConfig([string]$Path,[switch]$AllowEmpty){
    if(-not [IO.File]::Exists($Path)){throw 'Create config/devices.local.json first; see README.md and config/devices.example.json.'}
    $taskConfig=[IO.File]::ReadAllText($Path) | ConvertFrom-Json
    if($taskConfig.Version -ne 1){throw 'Unsupported device configuration version.'}
    $taskEntries=@($taskConfig.Devices)
    if(($taskEntries.Count -lt 1 -and -not $AllowEmpty) -or $taskEntries.Count -gt 3){throw 'Configure between one and three radio devices.'}
    $taskNames=@{};$taskMacs=@{};$taskProfiles=@{};$taskIndex=0;$taskDevices=@()
    $taskAutomatic=[bool]$taskConfig.AutoDetect
    foreach($taskEntry in $taskEntries){
        $taskName=[string]$taskEntry.Name
        $taskMac=([string]$taskEntry.Mac -replace '[:-]','').ToLowerInvariant()
        $taskProfile=[string]$taskEntry.Profile
        if($taskName -notmatch '^[a-z][a-z0-9-]{0,23}$' -or $taskNames.ContainsKey($taskName)){throw 'Each device needs a unique lowercase name (letters, digits, hyphens).'}
        if($taskMac -notmatch '^[0-9a-f]{12}$' -or ((-not $taskAutomatic -or $taskMac -ne '000000000000') -and ($taskMac -eq '000000000000' -or $taskMacs.ContainsKey($taskMac)))){throw 'Each device needs a unique six-byte radio MAC address.'}
        if($taskProfile -notin @('headset-0b32-usb.json','headset-0b17-usb.json','controller-0b12-usb.json')){throw 'Only the three captured device profiles are supported.'}
        if($taskProfiles.ContainsKey($taskProfile)){throw 'This preview supports one device per profile; duplicate virtual serials are not supported.'}
        $taskProfiles[$taskProfile]=$true
        $taskNames[$taskName]=$true;$taskMacs[$taskMac]=$true
        $taskDevices+=@{Name=$taskName;Mac=$taskMac;ProfileName=$taskProfile;Audio=$taskProfile.StartsWith('headset-');UsbPort=(33240+2*$taskIndex);RadioPort=(33241+2*$taskIndex);StatusName=('usb-gip-'+$taskName+'-radio-status.json')}
        $taskIndex++
    }
    $taskBackend=if($taskConfig.BridgeBackend){[string]$taskConfig.BridgeBackend}else{'python'}
    if($taskBackend -notin @('python','cpp')){throw 'BridgeBackend must be python or cpp.'}
    return [pscustomobject]@{AdapterInstanceId=[string]$taskConfig.AdapterInstanceId;Devices=$taskDevices;BridgeBackend=$taskBackend}
}
