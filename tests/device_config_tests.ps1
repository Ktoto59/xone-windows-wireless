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
    $taskConfig=Read-XoneDeviceConfig $taskPath
    Check ($taskConfig.Devices.Count -eq 3) 'Example configuration rejected'
    Check ((($taskConfig.Devices | ForEach-Object {$_.UsbPort}) -join ',') -eq '33240,33242,33244') 'Broker ports are not isolated'
    $taskGood=[IO.File]::ReadAllText($taskPath) | ConvertFrom-Json
    foreach($taskCase in @('duplicate-mac','duplicate-profile','profile-traversal','invalid-name','invalid-mac')){
        $taskBad=[IO.File]::ReadAllText($taskPath) | ConvertFrom-Json
        switch($taskCase){
            'duplicate-mac'{$taskBad.Devices[1].Mac=$taskBad.Devices[0].Mac}
            'duplicate-profile'{$taskBad.Devices[1].Profile=$taskBad.Devices[0].Profile}
            'profile-traversal'{$taskBad.Devices[0].Profile='../outside.json'}
            'invalid-name'{$taskBad.Devices[0].Name='../../outside'}
            'invalid-mac'{$taskBad.Devices[0].Mac='not-a-mac'}
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
