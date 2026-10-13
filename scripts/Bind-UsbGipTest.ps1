# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Device-Config.ps1')
$taskAdapterId=Resolve-XoneAdapterInstance
[void][IO.Directory]::CreateDirectory((Join-Path $taskRoot 'work'))
$taskResult=[ordered]@{Success=$false;Error=$null;Service=$null;ProblemCode=$null}
try{
    Start-Transcript -LiteralPath (Join-Path $taskRoot 'work\usb-gip-bind.log') -Force | Out-Null
    & (Join-Path $PSScriptRoot 'Switch-WinUSB.ps1') -Mode Bind
    $taskResult.Service=(Get-PnpDeviceProperty -InstanceId $taskAdapterId -KeyName DEVPKEY_Device_Service).Data
    $taskResult.ProblemCode=(Get-PnpDeviceProperty -InstanceId $taskAdapterId -KeyName DEVPKEY_Device_ProblemCode).Data
    $taskResult.Success=$taskResult.Service -eq 'WinUSB' -and $taskResult.ProblemCode -eq 0
}catch{$taskResult.Error=$_.Exception.Message}
finally{
    $taskResult | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskRoot 'work\usb-gip-bind-result.json') -Encoding UTF8
    try{Stop-Transcript | Out-Null}catch{}
}
