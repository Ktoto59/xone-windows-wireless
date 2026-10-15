# SPDX-License-Identifier: GPL-2.0-or-later
param([string]$PackageDirectory='')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $taskRoot 'scripts/Setup-Helpers.ps1')
function Check($Condition,[string]$Message){if(-not $Condition){throw $Message}}
$fixture=Join-Path $taskRoot ('work/stock-driver-test-'+[guid]::NewGuid().ToString('N'))
try{
    foreach($model in @('02FE','02E6','02F9','091E')){
        $instance='USB\VID_045E&PID_'+$model+'\EXAMPLE'
        $package=Get-XoneAdapterPackage $instance
        $expectedService=if($model -in @('02F9','091E')){'mt7612US_bc'}else{'mt7612US'}
        Check ($package.Service -eq $expectedService) 'Wrong model-specific stock service'
        Check ($package.Hash.Length -eq 64 -and $package.Url.StartsWith('https://catalog.s.download.windowsupdate.com/')) 'Unpinned or unofficial package'
        $saved=[pscustomobject]@{InstanceId=$instance;Service=$package.Service;InfName='oem123.inf';Section='MTWLAN.ndi'}
        Check (Test-XoneStockBinding $saved) 'Valid model-specific recovery state rejected'
        $saved.Service='unexpected'
        Check (-not (Test-XoneStockBinding $saved)) 'Unrecognized service accepted for restore'
        $script:fixtureService='WinUSB'
        function Get-XoneDeviceService($InstanceId){return $script:fixtureService}
        function Get-XoneDownload($Url,$Path,$Hash){throw 'Unexpected package download for an already prepared adapter'}
        Check ((Initialize-XoneStockDriver $fixture $instance) -eq 'WinUSB') 'Existing WinUSB was not preserved'
        $script:fixtureService=$package.Service
        Check ((Initialize-XoneStockDriver $fixture $instance) -eq $package.Service) 'Existing stock driver was not preserved'
    }
    if($PackageDirectory){
        $script:fixturePackages=[IO.Path]::GetFullPath($PackageDirectory)
        foreach($model in @('02FE','02E6','02F9','091E')){
            $instance='USB\VID_045E&PID_'+$model+'\EXAMPLE'
            $package=Get-XoneAdapterPackage $instance
            $script:fixtureModel=$model;$script:fixtureService='';$script:installCalls=0
            function Get-XoneDownload($Url,$Path,$Hash){
                $source=Join-Path $script:fixturePackages ($script:fixtureModel+'.cab')
                Check ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Hash) 'Official fixture CAB checksum mismatch'
                [void][IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
                Copy-Item -LiteralPath $source -Destination $Path -Force
            }
            function Invoke-XonePnputil([string[]]$Arguments){
                Check ($Arguments.Count -eq 3 -and $Arguments[0] -eq '/add-driver' -and $Arguments[2] -eq '/install') 'Unexpected driver installation command'
                Check ([IO.File]::Exists($Arguments[1])) 'Driver INF was not extracted before installation'
                $script:installCalls++
                $script:fixtureService=(Get-XoneAdapterPackage ('USB\VID_045E&PID_'+$script:fixtureModel+'\EXAMPLE')).Service
                return 0
            }
            $prepared=Initialize-XoneStockDriver $fixture $instance
            Check ($prepared -eq $package.Service -and $script:installCalls -eq 1) 'Missing stock driver was not installed automatically'
        }
        Write-Output 'PASS four real Microsoft CABs, catalog signatures, matching INFs and simulated missing-driver installation'
    }
    Write-Output 'PASS model-specific recovery validation and preservation of existing drivers'
}finally{
    if(Test-Path -LiteralPath $fixture){
        $resolved=[IO.Path]::GetFullPath($fixture)
        $boundary=[IO.Path]::GetFullPath((Join-Path $taskRoot 'work'))+[IO.Path]::DirectorySeparatorChar
        if(-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup outside work refused'}
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
