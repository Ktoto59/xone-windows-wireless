# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory=$true)][ValidateSet('Prepare','Bind','Restore','Remove','Upgrade','Start','Stop','Discover','AutomaticEnable','AutomaticDisable','UseCpp','UsePython')][string]$Action,
      [string]$InstanceId='',[string]$ResultPath='',[string]$DiscoveryPath='')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
$taskResult=[ordered]@{Success=$false;Action=$Action;Error=$null}
try{
    switch($Action){
        Prepare {Initialize-XonePrerequisites $taskRoot}
        Bind {
            Stop-XoneManagedBridge $taskRoot;Assert-XoneRadioIdle
            if(-not (Test-XoneFirmware $taskRoot)){throw 'Prepare the dependencies and firmware first.'}
            & (Join-Path $PSScriptRoot 'Switch-WinUSB.ps1') -Mode Bind -InstanceId $InstanceId
        }
        Restore {
            Stop-XoneManagedBridge $taskRoot;Assert-XoneRadioIdle
            & (Join-Path $PSScriptRoot 'Switch-WinUSB.ps1') -Mode Restore -InstanceId $InstanceId
        }
        Remove {& (Join-Path $PSScriptRoot 'Set-AutomaticBridge.ps1') -Mode Disable;Restore-XoneForRemoval $taskRoot}
        Upgrade {Assert-XonePanelClosed $taskRoot;Stop-XoneManagedBridge $taskRoot}
        Start {
            $usbip=Get-XoneUsbipStatus
            if(-not $usbip.Ready){throw 'Prepare USBip before starting the bridge.'}
            if(-not (Test-XoneFirmware $taskRoot)){throw 'Prepare the adapter firmware first.'}
            & (Join-Path $PSScriptRoot 'Set-AutomaticBridge.ps1') -Mode Start
            $deadline=(Get-Date).AddSeconds(20)
            do{
                Start-Sleep -Milliseconds 200
                $state=Read-XoneJson (Join-Path $taskRoot 'work/usb-gip-running.json')
                if($state -and $state.Error){throw $state.Error}
            }while((Get-Date) -lt $deadline -and (-not $state -or -not (Test-XoneProcess $state.Radio)))
            if(-not $state -or -not (Test-XoneProcess $state.Radio)){throw 'The bridge did not become ready. Inspect the work logs.'}
        }
        Stop {Stop-XoneManagedBridge $taskRoot}
        {$_ -in @('UseCpp','UsePython')} {
            $backend=if($Action -eq 'UseCpp'){'cpp'}else{'python'}
            if($backend -eq 'cpp' -and -not (Test-Path -LiteralPath (Join-Path $taskRoot 'outputs/xone-probe-cpp.exe'))){throw 'Build or install the C++ bridge first.'}
            [void](Read-XoneDeviceConfig (Join-Path $taskRoot 'config/devices.local.json'))
            Stop-XoneManagedBridge $taskRoot
            Set-XoneBridgeBackend $taskRoot $backend
            & (Join-Path $PSScriptRoot 'Set-AutomaticBridge.ps1') -Mode Start
        }
        AutomaticEnable {& (Join-Path $PSScriptRoot 'Set-AutomaticBridge.ps1') -Mode Enable}
        AutomaticDisable {& (Join-Path $PSScriptRoot 'Set-AutomaticBridge.ps1') -Mode Disable}
        Discover {
            Stop-XoneManagedBridge $taskRoot;Assert-XoneRadioIdle
            $InstanceId=Resolve-XoneAdapterInstance $InstanceId
            if((Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_Service).Data -ne 'WinUSB'){throw 'Select WinUSB and reconnect the adapter first.'}
            $stop=Join-Path $taskRoot 'work/discovery.stop'
            if(Test-Path -LiteralPath $stop){Remove-Item -LiteralPath $stop}
            if(-not $DiscoveryPath){throw 'Discovery requires a private output path.'}
            # The controller-test mode observes associations for all tested models.
            # Do not capture packet payloads or enable virtual outputs here.
            & (Join-Path $taskRoot 'outputs/xone-probe.exe') controller-test --instance $InstanceId --firmware (Join-Path $taskRoot 'work/firmware/FW_ACC_00U.bin') --seconds 90 --stop-file $stop *> $DiscoveryPath
            if($LASTEXITCODE -notin @(0,3)){throw ('Radio discovery failed (code '+$LASTEXITCODE+'). Inspect the private discovery log.')}
        }
    }
    $taskResult.Success=$true
}catch{$taskResult.Error=$_.Exception.Message;Write-Output $taskResult.Error}
finally{if($ResultPath){Write-XoneJson $ResultPath $taskResult}}
if(-not $taskResult.Success){exit 1}
