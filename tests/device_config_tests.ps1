$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $taskRoot 'scripts/Device-Config.ps1')
function Check($Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Get-PnpDevice {param([string]$InstanceId,[switch]$PresentOnly,[string]$ErrorAction) return [pscustomobject]@{InstanceId='USB\VID_045E&PID_02FE\EXAMPLE';Status='OK'}}
Check ((Resolve-XoneAdapterInstance) -eq 'USB\VID_045E&PID_02FE\EXAMPLE') 'Automatic adapter detection failed'
$taskDirectory=Join-Path $taskRoot ('work/config-test-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($taskDirectory)
$taskPath=Join-Path $taskDirectory 'devices.json'
try{
    Copy-Item -LiteralPath (Join-Path $taskRoot 'config/devices.example.json') -Destination $taskPath
    $taskAutomaticTemplate=Read-XoneDeviceConfig $taskPath -AllowEmpty
    Check ($taskAutomaticTemplate.Devices.Count -eq 0 -and $taskAutomaticTemplate.BridgeBackend -eq 'cpp') 'Public template requires device-specific mappings'
    $taskFixture=@{Version=1;AdapterInstanceId='';Devices=@(
        @{Name='new';Mac='020000000001';Profile='headset-0b32-usb.json'},
        @{Name='old';Mac='020000000002';Profile='headset-0b17-usb.json'},
        @{Name='controller';Mac='020000000003';Profile='controller-0b12-usb.json'})}
    [IO.File]::WriteAllText($taskPath,($taskFixture | ConvertTo-Json -Depth 5))
    $taskConfig=Read-XoneDeviceConfig $taskPath
    Check ($taskConfig.Devices.Count -eq 3) 'Example configuration rejected'
    Check ($taskConfig.BridgeBackend -eq 'python') 'Reference backend default changed'
    Check ((($taskConfig.Devices | ForEach-Object {$_.UsbPort}) -join ',') -eq '33240,33242,33244') 'Broker ports are not isolated'
    $taskGood=[IO.File]::ReadAllText($taskPath) | ConvertFrom-Json
    $taskCpp=$taskGood | ConvertTo-Json -Depth 5 | ConvertFrom-Json
    $taskCpp | Add-Member -NotePropertyName BridgeBackend -NotePropertyValue cpp
    $taskCppPath=Join-Path $taskDirectory 'cpp.json'
    [IO.File]::WriteAllText($taskCppPath,($taskCpp | ConvertTo-Json -Depth 5))
    Check ((Read-XoneDeviceConfig $taskCppPath).BridgeBackend -eq 'cpp') 'C++ backend selection rejected'
    $taskEmptyPath=Join-Path $taskDirectory 'empty.json'
    [IO.File]::WriteAllText($taskEmptyPath,(@{Version=1;AdapterInstanceId='';BridgeBackend='cpp';Devices=@()} | ConvertTo-Json))
    Check ((Read-XoneDeviceConfig $taskEmptyPath -AllowEmpty).Devices.Count -eq 0) 'Fresh first-run setup requires a device before pairing'
    $taskRejected=$false;try{[void](Read-XoneDeviceConfig $taskEmptyPath)}catch{$taskRejected=$true}
    Check $taskRejected 'A radio launcher accepted an empty mapping without AllowEmpty'
    foreach($taskCase in @('duplicate-mac','duplicate-profile','profile-traversal','invalid-name','invalid-mac','invalid-backend')){
        $taskBad=[IO.File]::ReadAllText($taskPath) | ConvertFrom-Json
        switch($taskCase){
            'duplicate-mac'{$taskBad.Devices[1].Mac=$taskBad.Devices[0].Mac}
            'duplicate-profile'{$taskBad.Devices[1].Profile=$taskBad.Devices[0].Profile}
            'profile-traversal'{$taskBad.Devices[0].Profile='../outside.json'}
            'invalid-name'{$taskBad.Devices[0].Name='../../outside'}
            'invalid-mac'{$taskBad.Devices[0].Mac='not-a-mac'}
            'invalid-backend'{$taskBad | Add-Member -NotePropertyName BridgeBackend -NotePropertyValue unknown}
        }
        $taskCasePath=Join-Path $taskDirectory ($taskCase+'.json')
        [IO.File]::WriteAllText($taskCasePath,($taskBad | ConvertTo-Json -Depth 5))
        $taskRejected=$false;try{[void](Read-XoneDeviceConfig $taskCasePath)}catch{$taskRejected=$true}
        Check $taskRejected ('Invalid configuration accepted: '+$taskCase)
    }
    function Get-PnpDevice {param([string]$InstanceId,[switch]$PresentOnly,[string]$ErrorAction) return @([pscustomobject]@{InstanceId='USB\VID_045E&PID_02FE\EXAMPLE-A'},[pscustomobject]@{InstanceId='USB\VID_045E&PID_02FE\EXAMPLE-B'})}
    $taskRejected=$false;try{[void](Resolve-XoneAdapterInstance)}catch{$taskRejected=$true}
    Check $taskRejected 'Ambiguous adapter selection accepted'
    Write-Output 'PASS config validation, private device mapping and adapter ambiguity'
}finally{
    foreach($taskFile in [IO.Directory]::GetFiles($taskDirectory)){[IO.File]::Delete($taskFile)}
    [IO.Directory]::Delete($taskDirectory)
}
