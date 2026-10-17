# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts/Adapter-IdlePower.ps1')
function Assert-Task($Value,[string]$Message){if(-not $Value){throw $Message}}
$taskRegistry='HKCU:\Software\xone-idle-power-test-'+[guid]::NewGuid().ToString('N')
try{
    New-Item -Path $taskRegistry | Out-Null
    New-ItemProperty -LiteralPath $taskRegistry -Name DeviceIdleEnabled -Value 0 -PropertyType DWord | Out-Null
    New-ItemProperty -LiteralPath $taskRegistry -Name DefaultIdleState -Value 1 -PropertyType DWord | Out-Null
    New-ItemProperty -LiteralPath $taskRegistry -Name UnrelatedSetting -Value 23 -PropertyType DWord | Out-Null
    $taskSaved=Save-XoneIdlePower $taskRegistry
    # Exercise the exact serialized recovery format used by binding-state.json.
    $taskSaved=$taskSaved | ConvertTo-Json -Depth 4 | ConvertFrom-Json
    Enable-XoneIdlePower $taskRegistry
    $taskPolicy=Get-ItemProperty -LiteralPath $taskRegistry
    Assert-Task ($taskPolicy.DeviceIdleEnabled -eq 1 -and $taskPolicy.DeviceIdleIgnoreWakeEnable -eq 0 -and $taskPolicy.DefaultIdleState -eq 0) 'Idle policy did not require wake-capable managed sleep'
    Restore-XoneIdlePower $taskRegistry $taskSaved
    $taskPolicy=Get-ItemProperty -LiteralPath $taskRegistry
    Assert-Task ($taskPolicy.DeviceIdleEnabled -eq 0 -and $taskPolicy.DefaultIdleState -eq 1) 'Original DWORD values were lost'
    Assert-Task (-not $taskPolicy.PSObject.Properties['DeviceIdleIgnoreWakeEnable']) 'Originally absent policy was retained'
    Assert-Task ($taskPolicy.UnrelatedSetting -eq 23) 'Unrelated registry value changed'
    New-ItemProperty -LiteralPath $taskRegistry -Name DeviceIdleEnabled -Value 'invalid' -PropertyType String -Force | Out-Null
    $taskRejected=$false;try{Save-XoneIdlePower $taskRegistry | Out-Null}catch{$taskRejected=$true}
    Assert-Task $taskRejected 'Unexpected registry type accepted for recovery'
    Write-Output 'Adapter idle power save/apply/serialized restore/isolation tests passed'
}finally{
    # Exact newly-created leaf; this test never enumerates or deletes adapters.
    if(Test-Path -LiteralPath $taskRegistry){Remove-Item -LiteralPath $taskRegistry -Force}
}
