# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Destination,[string]$CacheDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
if(-not $CacheDirectory){$CacheDirectory=Join-Path (Split-Path -Parent $PSScriptRoot) 'work/driver-catalog'}
[void][IO.Directory]::CreateDirectory($Destination)
$entries=@()
foreach($model in @('02FE','02E6','02F9','091E')){
    $instance='USB\VID_045E&PID_'+$model+'\PACKAGE'
    $package=Get-XoneAdapterPackage $instance
    $source=Join-Path $CacheDirectory ($model+'.cab')
    Get-XoneDownload $package.Url $source $package.Hash
    # Verify actual catalog signatures/hardware IDs from the unchanged CAB.
    $checkRoot=Join-Path (Split-Path -Parent $PSScriptRoot) ('work/stock-bundle-check/'+$model)
    $payload=Join-Path $checkRoot ('drivers/stock/'+$model+'.cab')
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $payload))
    Copy-Item -LiteralPath $source -Destination $payload -Force
    [void](Get-XonePreparedStockPackage $checkRoot $instance)
    Copy-Item -LiteralPath $source -Destination (Join-Path $Destination ($model+'.cab')) -Force
    $entries+=@{Model=$model;SHA256=$package.Hash;Source=$package.Url;Catalog=$package.Catalog;Inf=$package.Inf}
}
Write-XoneJson (Join-Path $Destination 'packages.json') @{Version=1;Packages=$entries}
Write-Output 'PASS four pinned Microsoft stock-driver CABs staged and verified for the runtime payload'
