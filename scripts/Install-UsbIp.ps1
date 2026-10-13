# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskInstaller = Join-Path $taskRoot 'work\USBip-0.9.8.1-x64.exe'
$taskLog = Join-Path $taskRoot 'work\usbip-install.log'
$taskResultPath = Join-Path $taskRoot 'work\usbip-install-result.json'
$taskResult = [ordered]@{StartedAt=(Get-Date).ToString('o'); Success=$false; ExitCode=$null; RebootRequested=$false; Error=$null; Log=$taskLog; Devices=@(); Packages=@(); Catalogs=@()}
try {
    $taskIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]$taskIdentity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrator privileges are required.' }
    $taskSignature = Get-AuthenticodeSignature -LiteralPath $taskInstaller
    if ($taskSignature.Status -ne 'Valid' -or $taskSignature.SignerCertificate.Subject -notmatch 'Scheibling Consulting AB') { throw 'Installer signature verification failed.' }
    if ((Get-FileHash -LiteralPath $taskInstaller -Algorithm SHA256).Hash -ne '38CAD6D4432B52D5BB9409D9AD03B72FDFFC4ADA4CD3A48FBECA1A2752A8518A') { throw 'Installer hash changed.' }
    $taskResult | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $taskResultPath -Encoding UTF8
    $taskArguments = '/VERYSILENT /SP- /SUPPRESSMSGBOXES /NORESTART /RESTARTEXITCODE=3010 /NOCLOSEAPPLICATIONS /NORESTARTAPPLICATIONS /COMPONENTS="main,client,sdk,pdb" /TASKS="" /LOG="' + $taskLog + '"'
    $taskProcess = Start-Process -FilePath $taskInstaller -ArgumentList $taskArguments -WindowStyle Hidden -PassThru -Wait
    $taskResult.ExitCode = $taskProcess.ExitCode
    $taskResult.RebootRequested = $taskProcess.ExitCode -eq 3010
    if ($taskProcess.ExitCode -notin @(0,3010)) { throw "USBip installer failed: $($taskProcess.ExitCode). Inspect the log; no signing policy changes are performed by this script." }
    $taskResult.Packages = @(Get-WindowsDriver -Online -All | Where-Object {$_.OriginalFileName -match 'usbip2_(ude|filter)\.inf$'} | Select-Object Driver,OriginalFileName,ProviderName,ClassName,Version)
    foreach ($taskPackage in $taskResult.Packages) {
        $taskCatalogFiles = @(Get-ChildItem -LiteralPath (Split-Path -Parent $taskPackage.OriginalFileName) -Filter '*.cat')
        foreach ($taskCatalog in $taskCatalogFiles) {
            $taskCatalogSignature = Get-AuthenticodeSignature -LiteralPath $taskCatalog.FullName
            $taskResult.Catalogs += [ordered]@{Path=$taskCatalog.FullName; Status=$taskCatalogSignature.Status.ToString(); Signer=$taskCatalogSignature.SignerCertificate.Subject; SHA256=(Get-FileHash -LiteralPath $taskCatalog.FullName -Algorithm SHA256).Hash}
        }
    }
    $taskResult.Devices = @(Get-PnpDevice -PresentOnly | Where-Object {$_.InstanceId -like 'ROOT\USB\*' -or $_.InstanceId -like 'USB\ROOT_HUB30*' -or $_.InstanceId -like 'USB\VID_045E*'} | ForEach-Object {
        $taskDevice = $_
        $taskProperties = @{}
        foreach ($taskKey in @('Service','ProblemCode','DriverInfPath','UpperFilters','LowerFilters')) {
            $taskProperty = Get-PnpDeviceProperty -InstanceId $taskDevice.InstanceId -KeyName ('DEVPKEY_Device_' + $taskKey) -ErrorAction SilentlyContinue
            $taskProperties[$taskKey] = $taskProperty.Data
        }
        [ordered]@{InstanceId=$taskDevice.InstanceId; Name=$taskDevice.FriendlyName; Status=$taskDevice.Status; Properties=$taskProperties}
    })
    if ($taskResult.Packages.Count -lt 2 -or $taskResult.Catalogs.Count -lt 2 -or @($taskResult.Catalogs | Where-Object {$_.Status -ne 'Valid'}).Count) { throw 'Both signed driver packages were not confirmed.' }
    # Hardware ID ROOT\USBIP_WIN2\UDE is not the instance ID ROOT\USB\0000.
    $taskUdeDevices = @($taskResult.Devices | Where-Object {$_.Properties.Service -eq 'usbip2_ude'})
    if (-not $taskUdeDevices.Count -or @($taskUdeDevices | Where-Object {$_.Properties.ProblemCode -ne 0}).Count) { throw 'Virtual controller is not ready; inspect the captured device state.' }
    $taskResult.Success = $true
} catch { $taskResult.Error = $_.Exception.Message }
finally {
    $taskResult.FinishedAt = (Get-Date).ToString('o')
    $taskResult | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $taskResultPath -Encoding UTF8
}
if (-not $taskResult.Success) { exit 1 }
