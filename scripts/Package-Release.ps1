# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidatePattern('^\d+\.\d+\.\d+(?:-[a-z0-9.]+)?$')][string]$Version='0.1.0-alpha.3')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskDist=Join-Path $taskRoot 'dist'
[void][IO.Directory]::CreateDirectory($taskDist)
$taskName='xone-windows-wireless-'+$Version+'-windows-x64'
$taskStage=Join-Path $taskDist $taskName
if(Test-Path -LiteralPath $taskStage){throw 'Package staging directory already exists; use a fresh version or remove your previous staging directory explicitly.'}
[void][IO.Directory]::CreateDirectory($taskStage)
Push-Location -LiteralPath $taskRoot
try{
    $taskFiles=@(& git ls-files)
    if($LASTEXITCODE -ne 0 -or -not $taskFiles.Count){throw 'Package from a Git source checkout.'}
    foreach($taskFile in $taskFiles){
        if($taskFile -match '^(work|outputs|build|dist|runtime)/' -or $taskFile -match '\.local\.json$'){throw 'Private or generated file unexpectedly tracked.'}
        $taskDestination=Join-Path $taskStage $taskFile
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $taskDestination))
        Copy-Item -LiteralPath (Join-Path $taskRoot $taskFile) -Destination $taskDestination
    }
    $taskOutput=Join-Path $taskStage 'outputs'
    [void][IO.Directory]::CreateDirectory($taskOutput)
    foreach($taskBinary in @('xone-probe.exe','xone-bind.exe','native-gip-probe.exe','native-audio-probe.exe','audio-endpoint-test.exe','usb-reference-dump.exe','etw-trace-dump.exe','protocol-tests.exe','session-tests.exe','xone-control.exe')){
        $taskSource=Join-Path $taskRoot ('outputs/'+$taskBinary)
        if(-not (Test-Path -LiteralPath $taskSource)){throw ('Missing release binary: '+$taskBinary)}
        Copy-Item -LiteralPath $taskSource -Destination $taskOutput
    }
    $taskManifest=[ordered]@{Project='xone-windows-wireless';Version=$Version;Architecture='x64';Commit=((& git rev-parse HEAD).Trim());BuiltAt=(Get-Date).ToUniversalTime().ToString('o');FirmwareIncluded=$false;MicrosoftDriversIncluded=$false;PrivateConfigurationIncluded=$false;Files=@()}
    foreach($taskFile in Get-ChildItem -LiteralPath $taskStage -File -Recurse){
        $taskRelative=$taskFile.FullName.Substring($taskStage.Length+1).Replace('\','/')
        $taskManifest.Files+=@{Path=$taskRelative;SHA256=(Get-FileHash -LiteralPath $taskFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant();Bytes=$taskFile.Length}
    }
    [IO.File]::WriteAllText((Join-Path $taskStage 'release-manifest.json'),($taskManifest | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
    $taskArchive=Join-Path $taskDist ($taskName+'.zip')
    Compress-Archive -LiteralPath $taskStage -DestinationPath $taskArchive -CompressionLevel Optimal
    Write-Output $taskArchive
}finally{Pop-Location}
