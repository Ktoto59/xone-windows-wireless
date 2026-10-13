# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([string]$NewHeadsetMac='',[string]$OldHeadsetMac='',[string]$ControllerMac='',[string]$AdapterInstanceId='')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Device-Config.ps1')
$taskDevices=@()
if($NewHeadsetMac){$taskDevices+=@{Name='new';Mac=$NewHeadsetMac;Profile='headset-0b32-usb.json'}}
if($OldHeadsetMac){$taskDevices+=@{Name='old';Mac=$OldHeadsetMac;Profile='headset-0b17-usb.json'}}
if($ControllerMac){$taskDevices+=@{Name='controller';Mac=$ControllerMac;Profile='controller-0b12-usb.json'}}
if(-not $taskDevices.Count){throw 'Provide at least one device MAC. Use the discovery command described in README.md.'}
$taskPath=Join-Path $taskRoot 'config/devices.local.json'
if(Test-Path -LiteralPath $taskPath){throw 'Local configuration already exists. Edit it manually; this helper will not overwrite it.'}
$taskJson=@{Version=1;AdapterInstanceId=$AdapterInstanceId;Devices=$taskDevices} | ConvertTo-Json -Depth 5
[void][IO.Directory]::CreateDirectory((Split-Path -Parent $taskPath))
[IO.File]::WriteAllText($taskPath,$taskJson,[Text.UTF8Encoding]::new($false))
try{[void](Read-XoneDeviceConfig $taskPath)}catch{Remove-Item -LiteralPath $taskPath;throw}
Write-Output 'Created config/devices.local.json. Keep this file private; Git ignores it.'
