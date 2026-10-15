# SPDX-License-Identifier: GPL-2.0-or-later
param([Parameter(Mandatory=$true)][string]$CabPath)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $taskRoot 'scripts/Setup-Helpers.ps1')
function Check($Condition,[string]$Message){if(-not $Condition){throw $Message}}
$fixture=Join-Path $taskRoot ('work/bootstrap-test-'+[guid]::NewGuid().ToString('N'))
$script:fixtureCab=[IO.Path]::GetFullPath($CabPath)
$script:fixtureRoot=$fixture;$script:fixtureService='';$script:usbipReady=$false
$script:downloads=0;$script:installs=0;$script:stockInstalls=0
$script:installerExitCode=0;$script:trustedInstaller=$true
foreach($dir in @('scripts','outputs')){[void][IO.Directory]::CreateDirectory((Join-Path $fixture $dir))}
[IO.File]::WriteAllText((Join-Path $fixture 'outputs/xone-probe-cpp.exe'),'fixture; never executed')
[IO.File]::WriteAllText((Join-Path $fixture 'scripts/Import-Firmware.ps1'),@'
param([string]$Path='')
if(-not $Path){throw 'No firmware on fresh system'}
$root=Split-Path -Parent $PSScriptRoot
$target=Join-Path $root 'work/firmware/FW_ACC_00U.bin'
[void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
Copy-Item -LiteralPath $Path -Destination $target
'@)
[IO.File]::WriteAllText((Join-Path $fixture 'scripts/Switch-WinUSB.ps1'),@'
param($Mode,$InstanceId)
$root=Split-Path -Parent $PSScriptRoot
foreach($required in @('work/firmware/FW_ACC_00U.bin','work/usbip-installed','work/stock-installed')){
    if(-not [IO.File]::Exists((Join-Path $root $required))){throw 'Binding attempted before all dependencies were prepared'}
}
[IO.File]::WriteAllText((Join-Path $root 'work/bound'),'fixture binding completed')
'@)
function Stop-XoneManagedBridge($Root){}
function Assert-XoneRadioIdle {}
function Get-PnpDevice {param($InstanceId,[switch]$PresentOnly,$ErrorAction) return [pscustomobject]@{InstanceId='USB\VID_045E&PID_02FE\EXAMPLE';Status='Unknown'}}
function Get-XoneDeviceService($InstanceId){return $script:fixtureService}
function Get-XoneUsbipStatus {
    return [pscustomobject]@{Path=(Join-Path $script:fixtureRoot 'work/usbip-installed');Version='0.9.8.1';Ready=$script:usbipReady}
}
function Get-XoneDownload($Url,$Path,$Hash){
    $script:downloads++
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    if($Url.EndsWith('.cab')){
        Check ((Get-FileHash -LiteralPath $script:fixtureCab -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Hash) 'CAB was not pinned'
        Copy-Item -LiteralPath $script:fixtureCab -Destination $Path -Force
    }else{
        Check ($Url -eq 'https://github.com/vadimgrn/usbip-win2/releases/download/v.0.9.8.1/USBip-0.9.8.1-x64.exe' -and $Hash -eq $script:XoneUsbipHash) 'Wrong USBip dependency'
        [IO.File]::WriteAllText($Path,'fixture; never executed')
    }
}
function Get-AuthenticodeSignature($LiteralPath){
    if($LiteralPath.EndsWith('.cat')){return Microsoft.PowerShell.Security\Get-AuthenticodeSignature -LiteralPath $LiteralPath}
    return [pscustomobject]@{Status=$(if($script:trustedInstaller){'Valid'}else{'NotTrusted'});SignerCertificate=[pscustomobject]@{Subject='CN=Fixture Scheibling Consulting AB'}}
}
function Start-Process {param($FilePath,$ArgumentList,$Verb,$WindowStyle,[switch]$PassThru)
    Check ($FilePath.EndsWith('USBip-0.9.8.1-x64.exe') -and $WindowStyle -eq 'Hidden' -and $Verb -eq 'RunAs') 'Unexpected process launch'
    Check (Test-XoneFirmware $script:fixtureRoot) 'USBip started before firmware verification'
    $script:installs++;$script:usbipReady=($script:installerExitCode -eq 0)
    [IO.File]::WriteAllText((Join-Path $script:fixtureRoot 'work/usbip-installed'),'fixture')
    $process=[pscustomobject]@{Handle=1;ExitCode=$script:installerExitCode}
    $process | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {}
    return $process
}
function Invoke-XonePnputil([string[]]$Arguments){
    if($Arguments[0] -eq '/add-driver'){
        Check ([IO.File]::Exists($Arguments[1])) 'Missing stock INF'
        $script:stockInstalls++;$script:fixtureService='mt7612US'
        [IO.File]::WriteAllText((Join-Path $script:fixtureRoot 'work/stock-installed'),'fixture')
    }else{Check ($Arguments[0] -eq '/restart-device') 'Unexpected PnP operation'}
    return 0
}
try{
    $prepared=Initialize-XoneApplication $fixture '' -AllowPendingRestart
    Check (-not $prepared.ReconnectRequired) 'Simulated adapter restart failed'
    Check ($script:installs -eq 1 -and $script:stockInstalls -eq 1) 'Fresh install omitted a missing dependency'
    Check ([IO.File]::Exists((Join-Path $fixture 'work/bound'))) 'Binding did not run after preparation'
    $config=Read-XoneJson (Join-Path $fixture 'config/devices.local.json')
    Check ($config.BridgeBackend -eq 'cpp' -and $config.Devices.Count -eq 0) 'Fresh configuration requires a headset or manual mapping'
    $before=$script:downloads;$script:fixtureService='WinUSB'
    [void](Initialize-XoneApplication $fixture '' -AllowPendingRestart)
    Check ($script:downloads -eq $before -and $script:installs -eq 1 -and $script:stockInstalls -eq 1) 'Prepared installation repeated dependency installs'
    $script:usbipReady=$false
    Initialize-XonePrerequisites $fixture
    Check ($script:installs -eq 2 -and $script:usbipReady) 'Incomplete tested USBip installation was not repaired automatically'
    $script:usbipReady=$false;$script:installerExitCode=3010
    Initialize-XonePrerequisites $fixture -AllowPendingRestart
    Check ([IO.File]::Exists((Join-Path $fixture 'work/setup-restart-required.json'))) 'Dependency reboot request was lost'
    $before=$script:installs;$script:trustedInstaller=$false
    $rejected=$false;try{Initialize-XonePrerequisites $fixture -AllowPendingRestart}catch{$rejected=$true}
    Check ($rejected -and $script:installs -eq $before -and $script:XoneSetupStage -eq 'UsbipSignatureCheck') 'Untrusted dependency installer was executed'
    Write-Output 'PASS automatic USBip repair, pending-reboot handling and refusal to execute an untrusted download'
    Write-Output 'PASS complete fresh-system preparation: no firmware, no USBip, no stock adapter driver; bind only after dependencies, repeat setup is idempotent'
}finally{
    $resolved=[IO.Path]::GetFullPath($fixture)
    $boundary=[IO.Path]::GetFullPath((Join-Path $taskRoot 'work'))+[IO.Path]::DirectorySeparatorChar
    if(-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup outside work refused'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
