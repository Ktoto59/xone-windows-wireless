# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $taskRoot 'scripts/Setup-Helpers.ps1')
function Check($Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Reject([scriptblock]$Operation,[string]$Message){$rejected=$false;try{& $Operation}catch{$rejected=$true};Check $rejected $Message}
$taskDirectory=Join-Path $taskRoot ('work/setup-test-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory((Join-Path $taskDirectory 'work'))
$taskLock=$null
try{
    $macs=@(Get-XoneDiscoveredMacs "ASSOCIATED WCID=1 MAC=020000000001`nASSOCIATED WCID=2 MAC=020000000001`nASSOCIATED WCID=3 MAC=020000000002`nMAC=invalid`n")
    Check ($macs.Count -eq 2 -and $macs[1] -eq '020000000002') 'Association discovery did not deduplicate observed MACs'
    Check (@(Get-XoneDiscoveredMacs 'not an association MAC=020000000003').Count -eq 0) 'Non-association text was accepted'
    $path=Join-Path $taskDirectory 'config/devices.local.json'
    $first=@([pscustomobject]@{Name='controller';Mac='020000000001';Profile='controller-0b12-usb.json'})
    Set-XoneSetupConfig $taskDirectory '' $first
    Check ((Read-XoneDeviceConfig $path).Devices[0].Mac -eq '020000000001') 'Valid config not saved'
    $invalid=@($first)+@([pscustomobject]@{Name='headset';Mac='020000000001';Profile='headset-0b32-usb.json'})
    Reject {Set-XoneSetupConfig $taskDirectory '' $invalid} 'Duplicate mapping was accepted'
    Check ((Read-XoneDeviceConfig $path).Devices.Count -eq 1) 'Invalid edit overwrote the working configuration'
    Reject {Quote-XonePath "bad`"path"} 'Quoted argument accepted'
    Reject {Quote-XonePath "bad`npath"} 'Multiline argument accepted'
    Check ((Quote-XonePath 'C:\a folder\file.ps1') -eq '"C:\a folder\file.ps1"') 'Argument quoting changed spaces'
    $lockPath=Join-Path $taskDirectory 'work/control-panel.lock'
    $taskLock=[IO.FileStream]::new($lockPath,[IO.FileMode]::Create,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    Reject {Assert-XonePanelClosed $taskDirectory} 'Uninstall did not reject a running panel'
    $taskLock.Dispose();$taskLock=$null;Assert-XonePanelClosed $taskDirectory
    $old=[pscustomobject]@{Pid=$PID;StartTicks=1}
    Check (-not (Test-XoneProcess $old)) 'Reused PID accepted without matching process start'
    # Test download verification without any network or executable installer.
    function Invoke-WebRequest {param([switch]$UseBasicParsing,$Uri,$OutFile,$TimeoutSec) [IO.File]::WriteAllText($OutFile,'test-download')}
    $download=Join-Path $taskDirectory 'work/verified.bin'
    Reject {Get-XoneDownload 'https://example.invalid/file' $download ('0'*64)} 'Wrong download hash accepted'
    Check (-not (Test-Path -LiteralPath $download)) 'Unverified download promoted to trusted path'
    Check (-not (Test-Path -LiteralPath ($download+'.partial'))) 'Rejected partial file retained'
    $expected=Join-Path $taskDirectory 'work/expected.bin';[IO.File]::WriteAllText($expected,'test-download')
    Get-XoneDownload 'https://example.invalid/file' $download (Get-FileHash -LiteralPath $expected -Algorithm SHA256).Hash.ToLowerInvariant()
    Check ((Get-FileHash -LiteralPath $download).Hash -eq (Get-FileHash -LiteralPath $expected).Hash) 'Verified download was not promoted'
    # Device restoration must fail closed if a saved adapter is unplugged.
    Write-XoneJson (Join-Path $taskDirectory 'work/binding-state.json') @{InstanceId='USB\VID_045E&PID_02FE\TEST'}
    function Get-PnpDevice {param($InstanceId,[switch]$PresentOnly,$ErrorAction) return $null}
    Reject {Restore-XoneForRemoval $taskDirectory} 'Uninstall accepted an unplugged adapter with owned recovery state'
    Write-Output 'PASS setup discovery, atomic config, checksum rejection, process identity and uninstall guards'
}finally{
    if($taskLock){$taskLock.Dispose()}
    $taskResolved=[IO.Path]::GetFullPath($taskDirectory)
    $taskBoundary=[IO.Path]::GetFullPath((Join-Path $taskRoot 'work'))+[IO.Path]::DirectorySeparatorChar
    if(-not $taskResolved.StartsWith($taskBoundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Test cleanup outside work refused'}
    Remove-Item -LiteralPath $taskResolved -Recurse -Force
}
