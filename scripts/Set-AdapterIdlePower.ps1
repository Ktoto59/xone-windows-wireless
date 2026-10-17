# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateSet('Status','Enable','Restore')][string]$Mode='Status',[string]$InstanceId='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Device-Config.ps1')
. (Join-Path $PSScriptRoot 'Adapter-IdlePower.ps1')
$taskRoot=Split-Path -Parent $PSScriptRoot
$InstanceId=Resolve-XoneAdapterInstance $InstanceId
$taskRegistry="HKLM:\SYSTEM\CurrentControlSet\Enum\$InstanceId\Device Parameters"
if($Mode -eq 'Status'){Save-XoneIdlePower $taskRegistry | ConvertTo-Json -Depth 4;return}
$taskPrincipal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Idle power changes require an administrator PowerShell session.'}
if(@(Get-CimInstance Win32_Process | Where-Object {$_.Name -match '^xone-(background|probe)'}).Count){throw 'Stop all bridges before changing adapter idle policy.'}
if((Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_Service).Data -ne 'WinUSB'){throw 'Idle policy experiment requires the selected adapter to use WinUSB.'}
$taskStatePath=Join-Path $taskRoot 'work/binding-state.json'
$taskSaved=Get-Content -LiteralPath $taskStatePath -Raw | ConvertFrom-Json
if($taskSaved.InstanceId -ne $InstanceId -or -not (Test-XoneStockBinding $taskSaved)){throw 'A matching stock driver recovery state is required.'}
if($Mode -eq 'Enable'){
    if(-not $taskSaved.PSObject.Properties['IdlePower']){
        $taskSaved | Add-Member -NotePropertyName IdlePower -NotePropertyValue (Save-XoneIdlePower $taskRegistry)
        $taskTemporary=$taskStatePath+'.tmp'
        $taskSaved | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $taskTemporary -Encoding UTF8
        Move-Item -LiteralPath $taskTemporary -Destination $taskStatePath -Force
    }
    try{Enable-XoneIdlePower $taskRegistry}catch{Restore-XoneIdlePower $taskRegistry $taskSaved.IdlePower;throw}
}else{
    if(-not $taskSaved.PSObject.Properties['IdlePower']){throw 'No saved idle policy exists.'}
    Restore-XoneIdlePower $taskRegistry $taskSaved.IdlePower
}
Write-Output 'Adapter-specific idle policy updated. Unplug/replug before starting the bridge.'
