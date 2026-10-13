# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateRange(1,300)][int]$Seconds = 30,
      [ValidateSet('radio','controller')][string]$TestMode = 'radio',
      [switch]$WaitForReadySignal,
      [switch]$VirtualXInput,
      [switch]$RestoreStock)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Device-Config.ps1')
$taskAdapterId=Resolve-XoneAdapterInstance
[void][IO.Directory]::CreateDirectory((Join-Path $taskRoot 'work'))
$taskWork = Join-Path $taskRoot 'work'
$taskStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$taskLog = Join-Path $taskWork "hardware-$taskStamp.log"
$taskResultPath = Join-Path $taskWork 'hardware-result.json'
$taskProbe = Join-Path $taskRoot 'outputs\xone-probe.exe'
$taskFirmware = Join-Path $taskWork 'firmware\FW_ACC_00U.bin'
$taskCapture = Join-Path $taskWork "radio-$taskStamp.jsonl"
$taskReadySignal = Join-Path $taskWork "ready-$taskStamp.signal"
$taskStopSignal = Join-Path $taskWork "stop-$taskStamp.signal"
$taskProbeLog = Join-Path $taskWork "probe-$taskStamp.log"
$taskProbeError = Join-Path $taskWork "probe-$taskStamp.err"
$taskResult = [ordered]@{ Stage='starting'; TestMode=$TestMode; VirtualXInput=[bool]$VirtualXInput; RestoreRequested=[bool]$RestoreStock; Success=$false; RestoreSuccess=$false; KeptWinUSB=$false; Log=$taskLog; ProbeLog=$taskProbeLog; ProbeError=$taskProbeError; Capture=$taskCapture; ReadySignal=$taskReadySignal; StopSignal=$taskStopSignal; Error=$null }
function Save-TaskResult { $taskResult | ConvertTo-Json | Set-Content -LiteralPath $taskResultPath -Encoding UTF8 }
New-Item -ItemType Directory -Force -Path $taskWork | Out-Null
Start-Transcript -LiteralPath $taskLog -Force | Out-Null
Push-Location -LiteralPath $taskRoot
try {
    $taskService = (Get-PnpDeviceProperty -InstanceId $taskAdapterId -KeyName 'DEVPKEY_Device_Service').Data
    $taskChanged = $taskService -ne 'WinUSB'
    if ($taskChanged) {
        $taskResult.Stage='binding'; Save-TaskResult
        & (Join-Path $PSScriptRoot 'Switch-WinUSB.ps1') -Mode Bind
        $taskResult.Stage='waiting-for-replug'; Save-TaskResult
        Write-Output 'Unplug and reconnect the adapter to the SAME USB port now. Waiting up to 180 seconds.'
    } else { Write-Output 'Reusing existing WinUSB binding.' }
    if ($WaitForReadySignal -and $taskChanged) {
        Write-Output "After completing the replug, create the confirmation file: $taskReadySignal"
        $taskSignalDeadline = (Get-Date).AddSeconds(300)
        while (-not (Test-Path -LiteralPath $taskReadySignal)) {
            if ((Get-Date) -gt $taskSignalDeadline) { throw 'Replug confirmation timed out.' }
            Start-Sleep -Seconds 1
        }
    }
    $taskDeadline = (Get-Date).AddSeconds(180)
    $taskReady = $false
    do {
        try {
            $ErrorActionPreference = 'Continue'
            $taskProbeOutput = & $taskProbe probe 2>&1
            $taskProbeExit = $LASTEXITCODE
        } finally { $ErrorActionPreference = 'Stop' }
        if ($taskProbeExit -eq 0) {
            $taskProbeOutput | Write-Output
            $taskReady = $true
            break
        }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $taskDeadline)
    if (-not $taskReady) { throw 'USB interface did not become ready after the replug wait.' }
    $taskResult.Stage="$TestMode-test"; Save-TaskResult
    if ($TestMode -eq 'controller') {
        $taskProbeArgs = 'controller-test --firmware "'+$taskFirmware+'" --seconds '+$Seconds+' --capture "'+$taskCapture+'" --stop-file "'+$taskStopSignal+'"'
        if ($VirtualXInput) { $taskProbeArgs += ' --virtual-xinput' }
    } else {
        $taskProbeArgs = 'radio-test --firmware "'+$taskFirmware+'" --seconds '+$Seconds+' --capture "'+$taskCapture+'" --verbose'
    }
    $taskChild = Start-Process -FilePath $taskProbe -ArgumentList $taskProbeArgs -RedirectStandardOutput $taskProbeLog -RedirectStandardError $taskProbeError -WindowStyle Hidden -PassThru -Wait
    Get-Content -LiteralPath $taskProbeLog | Write-Output
    Get-Content -LiteralPath $taskProbeError | Write-Output
    if ($taskChild.ExitCode -ne 0) { throw "$TestMode test failed (exit $($taskChild.ExitCode)). See $taskProbeError" }
    $taskResult.Success=$true
} catch {
    $taskResult.Error="$($_.Exception.Message)"
    Write-Warning $taskResult.Error
} finally {
    if ($RestoreStock) {
      $taskResult.Stage='restoring'; Save-TaskResult
      if (Test-Path -LiteralPath (Join-Path $taskWork 'binding-state.json')) {
        try {
            $taskRestoreDeadline = (Get-Date).AddSeconds(60)
            do {
                $taskPresent = Get-PnpDevice -InstanceId $taskAdapterId -PresentOnly -ErrorAction SilentlyContinue
                if ($taskPresent) { break }
                Start-Sleep -Seconds 2
            } while ((Get-Date) -lt $taskRestoreDeadline)
            if (-not $taskPresent) { throw 'Adapter absent during restoration; reconnect it and run Restore-Stock.ps1.' }
            & (Join-Path $PSScriptRoot 'Switch-WinUSB.ps1') -Mode Restore
            $taskService = (Get-PnpDeviceProperty -InstanceId $taskAdapterId -KeyName 'DEVPKEY_Device_Service').Data
            $taskStatus = (Get-PnpDevice -InstanceId $taskAdapterId).Status
            if ($taskService -ne 'mt7612US' -or $taskStatus -ne 'OK') { throw 'Stock service/status verification failed.' }
            $taskResult.RestoreSuccess=$true
        } catch {
            $taskResult.Error += " Restoration: $($_.Exception.Message)"
            Write-Warning $taskResult.Error
        }
      } else {
        $taskResult.RestoreSuccess=((Get-PnpDeviceProperty -InstanceId $taskAdapterId -KeyName 'DEVPKEY_Device_Service').Data -eq 'mt7612US')
      }
    } else {
        $taskResult.KeptWinUSB=((Get-PnpDeviceProperty -InstanceId $taskAdapterId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data -eq 'WinUSB')
        Write-Output 'Leaving the adapter on WinUSB for development. Explicit recovery: scripts\Restore-Stock.ps1.'
    }
    $taskResult.Stage='finished'; Save-TaskResult
    Pop-Location
    Stop-Transcript | Out-Null
}
if (-not $taskResult.Success -or ($RestoreStock -and -not $taskResult.RestoreSuccess)) { exit 1 }
