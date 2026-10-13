# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([string]$Path='')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskExpected='48084d9fa53b9bb04358f3bb127b7495dc8f7bb0b3ca1437bd24ef2b6eabdf66'
if($Path){$taskCandidates=@(Get-Item -LiteralPath $Path)}else{
    $taskStore=Join-Path $env:windir 'System32/DriverStore/FileRepository'
    $taskCandidates=@(Get-Item -LiteralPath (Join-Path $env:windir 'System32/drivers/FW_ACC_00U.bin') -ErrorAction SilentlyContinue)
    if(-not $taskCandidates.Count){
        $taskDriverFolders=@(Get-ChildItem -LiteralPath $taskStore -Directory -Filter 'mt7612us.inf_*' -ErrorAction SilentlyContinue)
        $taskCandidates=@($taskDriverFolders | ForEach-Object {Get-Item -LiteralPath (Join-Path $_.FullName 'FW_ACC_00U.bin') -ErrorAction SilentlyContinue})
    }
}
$taskMatch=$null
foreach($taskCandidate in $taskCandidates){
    if((Get-FileHash -LiteralPath $taskCandidate.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -eq $taskExpected){$taskMatch=$taskCandidate;break}
}
if(-not $taskMatch){throw 'No matching firmware found. See docs/INSTALL.md for the upstream Microsoft CAB extraction method.'}
$taskDestination=Join-Path $taskRoot 'work/firmware/FW_ACC_00U.bin'
[void][IO.Directory]::CreateDirectory((Split-Path -Parent $taskDestination))
if([IO.Path]::GetFullPath($taskMatch.FullName) -ne [IO.Path]::GetFullPath($taskDestination)){Copy-Item -LiteralPath $taskMatch.FullName -Destination $taskDestination -Force}
if((Get-FileHash -LiteralPath $taskDestination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskExpected){throw 'Copied firmware hash mismatch.'}
Write-Output 'Verified firmware imported locally. Firmware is not included in this project or release archive.'
