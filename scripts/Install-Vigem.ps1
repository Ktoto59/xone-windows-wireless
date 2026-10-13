# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskInstaller=Join-Path $taskRoot 'work\downloads\ViGEmBus_1.22.0_x64_x86_arm64.exe'
$taskResultPath=Join-Path $taskRoot 'work\vigem-install-result.json'
$taskLog=Join-Path $taskRoot 'work\vigem-install.log'
$taskResult=[ordered]@{Success=$false;ExitCode=$null;Error=$null;Log=$taskLog}
try {
    $taskSignature=Get-AuthenticodeSignature -LiteralPath $taskInstaller
    if($taskSignature.Status -ne 'Valid' -or $taskSignature.SignerCertificate.Subject -notmatch 'Nefarius Software Solutions') {throw 'Installer signature verification failed.'}
    if((Get-FileHash -LiteralPath $taskInstaller -Algorithm SHA256).Hash -ne '89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A') {throw 'Installer hash changed.'}
    $taskArguments='/quiet /norestart /log "'+$taskLog+'"'
    $taskChild=Start-Process -FilePath $taskInstaller -ArgumentList $taskArguments -WindowStyle Hidden -PassThru -Wait
    $taskResult.ExitCode=$taskChild.ExitCode
    if($taskChild.ExitCode -notin @(0,3010)) {throw "ViGEmBus installation failed: $($taskChild.ExitCode)"}
    $taskDevices=@(Get-PnpDevice -PresentOnly -Class System | Where-Object {$_.FriendlyName -eq 'Nefarius Virtual Gamepad Emulation Bus'} | Where-Object {(Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_Service').Data -eq 'ViGEmBus'})
    if(-not $taskDevices.Count -or @($taskDevices | Where-Object {$_.Status -ne 'OK'}).Count) {throw 'Installed ViGEmBus is not ready.'}
    $taskResult.Success=$true
} catch {$taskResult.Error=$_.Exception.Message}
finally {$taskResult | ConvertTo-Json | Set-Content -LiteralPath $taskResultPath -Encoding UTF8}
if(-not $taskResult.Success){exit 1}
