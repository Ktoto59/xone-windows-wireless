# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $taskRoot 'scripts/Native-Config.ps1')
function Check($Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Reject([scriptblock]$Operation){$rejected=$false;try{& $Operation}catch{$rejected=$true};Check $rejected 'Invalid native configuration was accepted'}
$taskDirectory=Join-Path $taskRoot ('work/native-config-test-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($taskDirectory)
$taskAdapter='USB\VID_045E&PID_02FE\EXAMPLE'
try{
    $path=Join-Path $taskDirectory 'config/native.local.json'
    $config=Initialize-XoneNativeConfig $taskDirectory $taskAdapter
    Check ($config.Peers.Count -eq 0) 'Fresh native configuration requires USB profiles'
    Save-XoneNativePeers $taskDirectory $config ([pscustomobject]@{Backend='native-gip';KnownPeers=@('020000000001')})
    Check ((Read-XoneNativeConfig $path).Peers.Count -eq 1) 'Paired native peer was not retained'
    $before=[IO.File]::ReadAllText($path)
    Reject {Save-XoneNativePeers $taskDirectory $config ([pscustomobject]@{Backend='native-gip';KnownPeers=@()})}
    Reject {Save-XoneNativePeers $taskDirectory $config ([pscustomobject]@{Backend='native-gip';KnownPeers=@('020000000001','../../invalid')})}
    Check ([IO.File]::ReadAllText($path) -eq $before) 'Bad status damaged pairing data'
    Reject {Initialize-XoneNativeConfig $taskDirectory 'USB\VID_045E&PID_02FE\OTHER'}
    foreach($peers in @(@('000000000000'),@('020000000001','020000000001'),@(1..17 | ForEach-Object {'02000000'+$_.ToString('x4')}))){
        Write-XoneJson $path @{Version=1;AdapterInstanceId=$taskAdapter;Peers=$peers};Reject {Read-XoneNativeConfig $path}
    }
    Remove-Item -LiteralPath $path
    Set-XoneSetupConfig $taskDirectory $taskAdapter @([pscustomobject]@{Name='controller';Mac='020000000001';Profile='controller-0b12-usb.json'})
    $usbPath=Join-Path $taskDirectory 'config/devices.local.json';$usbBefore=[IO.File]::ReadAllText($usbPath)
    $config=Initialize-XoneNativeConfig $taskDirectory $taskAdapter
    Check ($config.Peers.Count -eq 1 -and $config.Peers[0] -eq '020000000001') 'Existing paired device was not seeded'
    Check ([IO.File]::ReadAllText($usbPath) -eq $usbBefore) 'Native migration changed the USB fallback configuration'
    Write-Output 'PASS native configuration: empty start, pairing persistence, invalid snapshots, adapter ownership and fallback preservation'
}finally{
    $resolved=[IO.Path]::GetFullPath($taskDirectory);$boundary=[IO.Path]::GetFullPath((Join-Path $taskRoot 'work'))+[IO.Path]::DirectorySeparatorChar
    if(-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing cleanup outside work'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
