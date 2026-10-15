# SPDX-License-Identifier: GPL-2.0-or-later
param([string]$CabPath='')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $taskRoot 'scripts/Setup-Helpers.ps1')
function Check($Condition,[string]$Message){if(-not $Condition){throw $Message}}
$fixture=Join-Path $taskRoot ('work/prerequisite-test-'+[guid]::NewGuid().ToString('N'))
foreach($dir in @('scripts','outputs')){[void][IO.Directory]::CreateDirectory((Join-Path $fixture $dir))}
[IO.File]::WriteAllText((Join-Path $fixture 'outputs/xone-probe-cpp.exe'),'fixture; never executed')
[IO.File]::WriteAllText((Join-Path $fixture 'scripts/Import-Firmware.ps1'),@'
param([string]$Path='')
if(-not $Path){throw 'No existing firmware on this simulated fresh PC'}
$root=Split-Path -Parent $PSScriptRoot
$target=Join-Path $root 'work/firmware/FW_ACC_00U.bin'
if((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne "48084d9fa53b9bb04358f3bb127b7495dc8f7bb0b3ca1437bd24ef2b6eabdf66"){throw 'Wrong firmware'}
[void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
Copy-Item -LiteralPath $Path -Destination $target
'@)
try{
    function Get-XoneDownload([string]$Url,[string]$Path,[string]$Hash){
        Check ($Url.StartsWith('https://catalog.s.download.windowsupdate.com/')) 'Incorrect firmware HTTPS host'
        Check ($Hash -eq $script:XoneFirmwareCabHash -and $Hash.Length -eq 64) 'Firmware CAB not pinned'
        throw 'Simulated network failure'
    }
    $failed=$false
    try{Initialize-XonePrerequisites $fixture}catch{Check ($_.Exception.Message -eq 'Simulated network failure') 'Unexpected fresh-install error';$failed=$true}
    Check ($failed -and $script:XoneSetupStage -eq 'FirmwareDownload') 'Fresh install skipped download or lost stage'
    if($CabPath){
        $script:fixtureCab=[IO.Path]::GetFullPath($CabPath)
        function Get-XoneDownload([string]$Url,[string]$Path,[string]$Hash){
            Check ((Get-FileHash -LiteralPath $script:fixtureCab -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Hash) 'Integration CAB checksum mismatch'
            [void][IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
            Copy-Item -LiteralPath $script:fixtureCab -Destination $Path
        }
        function Get-XoneUsbipStatus {return [pscustomobject]@{Path=$PSHOME;Ready=$true;Version='0.9.8.1'}}
        Initialize-XonePrerequisites $fixture | Out-Null
        Check (Test-XoneFirmware $fixture) 'Fresh CAB extraction/import did not produce verified firmware'
        Write-Output 'PASS real Microsoft CAB extraction and fresh-directory firmware import'
    }
    Write-Output 'PASS fresh-install download selection, pinned CAB and failure stage'
}finally{
    $resolved=[IO.Path]::GetFullPath($fixture)
    $boundary=[IO.Path]::GetFullPath((Join-Path $taskRoot 'work'))+[IO.Path]::DirectorySeparatorChar
    if(-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup outside work refused'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
