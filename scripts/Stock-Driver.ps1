# SPDX-License-Identifier: GPL-2.0-or-later
# Verified model packages only. Dot-sourcing performs no IO or driver changes.
function Get-XoneBundledStockCab([string]$Root,[string]$InstanceId){
    $package=Get-XoneAdapterPackage $InstanceId
    $cab=Join-Path $Root ('drivers/stock/'+$package.Model+'.cab')
    if(-not [IO.File]::Exists($cab)){return $null}
    if((Get-Item -LiteralPath $cab -Force).LinkType -in @('SymbolicLink','Junction') -or
       (Get-Item -LiteralPath (Split-Path -Parent $cab) -Force).LinkType -in @('SymbolicLink','Junction')){throw 'Linked stock-driver payload refused.'}
    if((Get-FileHash -LiteralPath $cab -Algorithm SHA256).Hash.ToLowerInvariant() -ne $package.Hash){throw 'Bundled stock-driver checksum mismatch. Reinstall the application.'}
    return $cab
}
function Get-XonePreparedStockPackage([string]$Root,[string]$InstanceId,[switch]$AllowDownload){
    $package=Get-XoneAdapterPackage $InstanceId
    $cab=Get-XoneBundledStockCab $Root $InstanceId
    if(-not $cab){
        if(-not $AllowDownload){throw 'The bundled stock-driver package is missing. Run the current installer again; offline restoration cannot download a replacement.'}
        $cabName=if($package.Model -eq '02FE'){'adapter-driver.cab'}else{'adapter-'+$package.Model+'.cab'}
        $cab=Join-Path $Root ('work/downloads/'+$cabName)
        Get-XoneDownload $package.Url $cab $package.Hash
    }
    $directory=Join-Path $Root ('work/downloads/stock-'+$package.Model+'-'+$package.Hash)
    [void][IO.Directory]::CreateDirectory($directory)
    & (Get-XoneSystemTool 'expand.exe') '-F:*' $cab $directory | Out-Host
    if($LASTEXITCODE -ne 0){throw 'Cannot extract the verified Microsoft stock-driver package.'}
    $catalog=Join-Path $directory $package.Catalog
    $signature=Get-AuthenticodeSignature -LiteralPath $catalog
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=Microsoft Windows Hardware Compatibility Publisher,'){throw 'Microsoft stock-driver catalog verification failed.'}
    $inf=Join-Path $directory $package.Inf
    if(-not [IO.File]::ReadAllText($inf).Contains(('USB\VID_045E&PID_'+$package.Model))){throw 'Stock-driver hardware ID mismatch.'}
    return [pscustomobject]@{Inf=$inf;Directory=$directory;Package=$package}
}
function Get-XonePublishedStockInf([string]$Hash){
    $directory=Join-Path $env:windir 'INF'
    foreach($file in Get-ChildItem -LiteralPath $directory -File -Filter 'oem*.inf'){
        if((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Hash){return $file.FullName}
    }
    throw 'Windows did not publish the verified stock-driver INF.'
}
function Resolve-XoneRestoreDriver([string]$Root,$Saved){
    if(-not (Test-XoneStockBinding $Saved)){throw 'Invalid adapter recovery identity.'}
    [void](Get-XoneRecoveryBackupRoot $Root $Saved)
    foreach($name in @('UpperFilters','InterfaceGuids')){
        if($Saved.$name.Present -isnot [bool] -or -not $Saved.$name.PSObject.Properties['Value']){throw 'Original registry snapshots are missing; automatic restoration refused.'}
    }
    $package=Get-XoneAdapterPackage $Saved.InstanceId
    $inf=Get-XoneRecoveryInf $Saved
    if([IO.File]::Exists($inf)){
        $hash=(Get-FileHash -LiteralPath $inf -Algorithm SHA256).Hash.ToLowerInvariant()
        if([IO.File]::ReadAllText($inf).Contains(('USB\VID_045E&PID_'+$package.Model)) -and
           (-not $Saved.BackupInfSHA256 -or $Saved.BackupInfSHA256 -eq $hash)){
            return [pscustomobject]@{Inf=$inf;State=$Saved;Repaired=$false}
        }
    }
    # The original OEM package may have been removed. Stage a pinned signed
    # package without /install, then bind ONLY the selected instance via xone-bind.
    $prepared=Get-XonePreparedStockPackage $Root $Saved.InstanceId
    $code=Invoke-XonePnputil @('/add-driver',$prepared.Inf)
    if($code -notin @(0,3010)){throw ('Cannot stage the bundled stock driver (code '+$code+').')}
    if($code -eq 3010){Write-XoneJson (Join-Path $Root 'work/setup-restart-required.json') @{Required=$true;Reason='Stock driver staging'}}
    $hash=(Get-FileHash -LiteralPath $prepared.Inf -Algorithm SHA256).Hash.ToLowerInvariant()
    $published=Get-XonePublishedStockInf $hash
    $state=$Saved | ConvertTo-Json -Depth 10 | ConvertFrom-Json
    $state.InfName=[IO.Path]::GetFileName($published)
    $state | Add-Member -NotePropertyName BackupInfSHA256 -NotePropertyValue $hash -Force
    $state | Add-Member -NotePropertyName RecoveryPackageSource -NotePropertyValue 'BundledMicrosoft' -Force
    $backup=Get-XoneRecoveryBackupRoot $Root $state
    [void][IO.Directory]::CreateDirectory($backup)
    # Use the same driver-store export layout as Bind. A flat manual copy here
    # would duplicate INF files when the next Start exports this package again.
    $export=Invoke-XonePnputil @('/export-driver',$state.InfName,$backup)
    if($export -ne 0){throw ('Cannot save the staged stock-driver recovery package (code '+$export+').')}
    $infs=@(Get-ChildItem -LiteralPath $backup -Recurse -File -Filter $package.Inf)
    if($infs.Count -ne 1 -or (Get-FileHash -LiteralPath $infs[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash -or
       -not @(Get-ChildItem -LiteralPath $backup -Recurse -File -Filter $package.Catalog).Count -or
       -not @(Get-ChildItem -LiteralPath $backup -Recurse -File -Filter '*.sys').Count){throw 'Exported stock-driver package verification failed.'}
    return [pscustomobject]@{Inf=$published;State=$state;Repaired=$true}
}
