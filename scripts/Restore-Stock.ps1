# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Device-Config.ps1')
$taskAdapterId=Resolve-XoneAdapterInstance
[void][IO.Directory]::CreateDirectory((Join-Path $taskRoot 'work'))
$taskResultPath = Join-Path $taskRoot 'work\restore-result.json'
$taskLog = Join-Path $taskRoot ('work\restore-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$taskResult = [ordered]@{ Success=$false; Log=$taskLog; Error=$null }
Start-Transcript -LiteralPath $taskLog -Force | Out-Null
try {
    & (Join-Path $PSScriptRoot 'Switch-WinUSB.ps1') -Mode Restore
    $taskDevice = Get-PnpDevice -InstanceId $taskAdapterId
    $taskService = (Get-PnpDeviceProperty -InstanceId $taskDevice.InstanceId -KeyName DEVPKEY_Device_Service).Data
    if ($taskDevice.Status -ne 'OK' -or $taskService -ne 'mt7612US') { throw 'Stock driver/status verification failed.' }
    $taskResult.Success=$true
} catch { $taskResult.Error="$($_.Exception.Message)"; Write-Warning $taskResult.Error }
finally {
    $taskResult | ConvertTo-Json | Set-Content -LiteralPath $taskResultPath -Encoding UTF8
    Stop-Transcript | Out-Null
}
if (-not $taskResult.Success) { exit 1 }
