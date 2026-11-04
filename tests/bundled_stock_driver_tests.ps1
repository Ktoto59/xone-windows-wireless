# SPDX-License-Identifier: GPL-2.0-or-later
param([Parameter(Mandatory=$true)][string]$PackageDirectory)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $taskRoot 'scripts/Setup-Helpers.ps1')
function Check($Value,[string]$Message){if(-not $Value){throw $Message}}
function Reject([scriptblock]$Operation,[string]$Message){$failed=$false;try{& $Operation | Out-Null}catch{$failed=$true};Check $failed $Message}
$fixture=Join-Path $taskRoot ('work/bundled-stock-test-'+[guid]::NewGuid().ToString('N'))
$script:publishedDirectory=Join-Path $fixture 'published'
$script:staged=0
function Get-XoneDownload {throw 'Offline test attempted a download'}
function Get-XoneRecoveryInf($State){return (Join-Path $script:publishedDirectory $State.InfName)}
function Invoke-XonePnputil([string[]]$Arguments){
    if($Arguments[0] -eq '/export-driver'){
        Check ($Arguments.Count -eq 3 -and $Arguments[1] -eq 'oem999.inf') 'Export selected an unrelated driver'
        $destination=Join-Path $Arguments[2] 'fixture-package';[void][IO.Directory]::CreateDirectory($destination)
        Get-ChildItem -LiteralPath $script:preparedSource -File | Copy-Item -Destination $destination -Force
        return 0
    }
    Check ($Arguments.Count -eq 2 -and $Arguments[0] -eq '/add-driver') 'Staging attempted /install or a binding operation'
    $script:preparedSource=Split-Path -Parent $Arguments[1]
    [void][IO.Directory]::CreateDirectory($script:publishedDirectory)
    Copy-Item -LiteralPath $Arguments[1] -Destination (Join-Path $script:publishedDirectory 'oem999.inf') -Force
    $script:staged++;return 0
}
function Get-XonePublishedStockInf([string]$Hash){
    $path=Join-Path $script:publishedDirectory 'oem999.inf'
    Check ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Hash) 'Published INF differs'
    return $path
}
try{
    [void][IO.Directory]::CreateDirectory((Join-Path $fixture 'drivers/stock'))
    foreach($model in @('02FE','02E6','02F9','091E')){
        Copy-Item -LiteralPath (Join-Path $PackageDirectory ($model+'.cab')) -Destination (Join-Path $fixture ('drivers/stock/'+$model+'.cab'))
        $instance='USB\VID_045E&PID_'+$model+'\EXAMPLE'
        $prepared=Get-XonePreparedStockPackage $fixture $instance
        Check ([IO.File]::Exists($prepared.Inf) -and $prepared.Package.Model -eq $model) 'Wrong bundled model selected'
    }
    $instance='USB\VID_045E&PID_02FE\EXAMPLE';$package=Get-XoneAdapterPackage $instance
    $saved=[pscustomobject]@{InstanceId=$instance;Service=$package.Service;InfName='oem1.inf';Section=$package.Section;
        UpperFilters=[pscustomobject]@{Present=$true;Value=@('xboxgip','retained-filter')};InterfaceGuids=[pscustomobject]@{Present=$false;Value=@()}}
    $restore=Resolve-XoneRestoreDriver $fixture $saved
    Check ($restore.Repaired -and $script:staged -eq 1 -and $saved.InfName -eq 'oem1.inf') 'Fallback mutated original state or did not stage'
    Check ($restore.State.InfName -eq 'oem999.inf' -and $restore.State.UpperFilters.Value[1] -eq 'retained-filter') 'Fallback lost registry ownership snapshots'
    Write-XoneJson (Join-Path $fixture 'work/binding-state.json') $restore.State
    [void](Assert-XoneRecovery $fixture $instance)
    [void](Invoke-XonePnputil @('/export-driver','oem999.inf',(Get-XoneRecoveryBackupRoot $fixture $restore.State)))
    [void](Assert-XoneRecovery $fixture $instance) # A subsequent Bind export does not duplicate the package.
    $again=Resolve-XoneRestoreDriver $fixture $restore.State
    Check (-not $again.Repaired -and $script:staged -eq 1) 'Verified published driver was unnecessarily staged again'
    $destination=Join-Path $fixture 'migrated'
    Copy-XoneRecovery $fixture $destination $instance
    [void](Assert-XoneRecovery $destination $instance)
    Check ((Get-XoneRecoveryBackupRoot $destination $restore.State).EndsWith($restore.State.BackupInfSHA256)) 'Hash-scoped backup was not migrated'
    $invalid=$saved | ConvertTo-Json -Depth 6 | ConvertFrom-Json;$invalid.UpperFilters=$null
    Reject {Resolve-XoneRestoreDriver $fixture $invalid} 'Missing original registry metadata was silently invented'
    $invalid=$saved | ConvertTo-Json -Depth 6 | ConvertFrom-Json;$invalid.Service='WinUSB'
    Reject {Resolve-XoneRestoreDriver $fixture $invalid} 'Unknown original binding accepted'
    $cab=Join-Path $fixture 'drivers/stock/02FE.cab'
    [IO.File]::WriteAllText($cab,'corrupt package')
    Reject {Get-XoneBundledStockCab $fixture $instance} 'Tampered CAB accepted'
    [IO.File]::Delete($cab)
    Reject {Get-XonePreparedStockPackage $fixture $instance} 'Offline mode downloaded a missing CAB'
    Write-Output 'PASS four authentic CABs offline, signatures/HWIDs, staging-only fallback, retained snapshots, hash-scoped migration, corruption and ownership guards; no real drivers touched'
}finally{
    $resolved=[IO.Path]::GetFullPath($fixture);$boundary=[IO.Path]::GetFullPath((Join-Path $taskRoot 'work'))+[IO.Path]::DirectorySeparatorChar
    if(-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup outside work refused'}
    if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
