# SPDX-License-Identifier: GPL-2.0-or-later
function Resolve-XoneAdapterInstance([string]$InstanceId=''){
    if($InstanceId){
        if($InstanceId -notmatch '^USB\\VID_045E&PID_02FE\\[^\\\r\n]+$'){throw 'Only the external 045E:02FE adapter is supported by the binding scripts.'}
        [void](Get-PnpDevice -InstanceId $InstanceId -PresentOnly -ErrorAction Stop)
        return $InstanceId
    }
    $taskAdapters=@(Get-PnpDevice -PresentOnly | Where-Object {$_.InstanceId -match '^USB\\VID_045E&PID_02FE\\[^\\]+$'})
    if($taskAdapters.Count -ne 1){throw 'Expected exactly one external 045E:02FE adapter. Provide -InstanceId or AdapterInstanceId when more than one is connected.'}
    return $taskAdapters[0].InstanceId
}
function Read-XoneDeviceConfig([string]$Path){
    if(-not [IO.File]::Exists($Path)){throw 'Create config/devices.local.json first; see README.md and config/devices.example.json.'}
    $taskConfig=[IO.File]::ReadAllText($Path) | ConvertFrom-Json
    if($taskConfig.Version -ne 1){throw 'Unsupported device configuration version.'}
    $taskEntries=@($taskConfig.Devices)
    if($taskEntries.Count -lt 1 -or $taskEntries.Count -gt 3){throw 'Configure between one and three radio devices.'}
    $taskNames=@{};$taskMacs=@{};$taskProfiles=@{};$taskIndex=0;$taskDevices=@()
    foreach($taskEntry in $taskEntries){
        $taskName=[string]$taskEntry.Name
        $taskMac=([string]$taskEntry.Mac -replace '[:-]','').ToLowerInvariant()
        $taskProfile=[string]$taskEntry.Profile
        if($taskName -notmatch '^[a-z][a-z0-9-]{0,23}$' -or $taskNames.ContainsKey($taskName)){throw 'Each device needs a unique lowercase name (letters, digits, hyphens).'}
        if($taskMac -notmatch '^[0-9a-f]{12}$' -or $taskMac -eq '000000000000' -or $taskMacs.ContainsKey($taskMac)){throw 'Each device needs a unique six-byte radio MAC address.'}
        if($taskProfile -notin @('headset-0b32-usb.json','headset-0b17-usb.json','controller-0b12-usb.json')){throw 'Only the three captured device profiles are supported.'}
        if($taskProfiles.ContainsKey($taskProfile)){throw 'This preview supports one device per profile; duplicate virtual serials are not supported.'}
        $taskProfiles[$taskProfile]=$true
        $taskNames[$taskName]=$true;$taskMacs[$taskMac]=$true
        $taskDevices+=@{Name=$taskName;Mac=$taskMac;ProfileName=$taskProfile;Audio=$taskProfile.StartsWith('headset-');UsbPort=(33240+2*$taskIndex);RadioPort=(33241+2*$taskIndex);StatusName=('usb-gip-'+$taskName+'-radio-status.json')}
        $taskIndex++
    }
    return [pscustomobject]@{AdapterInstanceId=[string]$taskConfig.AdapterInstanceId;Devices=$taskDevices}
}
