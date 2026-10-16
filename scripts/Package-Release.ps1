# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidatePattern('^\d+\.\d+\.\d+(?:-[a-z0-9.]+)?$')][string]$Version='0.1.0-alpha.5.4',
      [string]$LauncherDir='')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskDist=Join-Path $taskRoot 'dist'
[void][IO.Directory]::CreateDirectory($taskDist)
$taskName='xone-windows-wireless-'+$Version+'-windows-x64'
$taskStage=Join-Path $taskDist $taskName
$taskSourceStage=Join-Path $taskDist ('xone-windows-wireless-'+$Version+'-source')
foreach($taskDirectory in @($taskStage,$taskSourceStage)){
    if(Test-Path -LiteralPath $taskDirectory){throw 'Package staging directory already exists; choose a fresh version or remove that exact previous stage explicitly.'}
    [void][IO.Directory]::CreateDirectory($taskDirectory)
}
function Copy-PackageFile([string]$Relative,[string]$Destination){
    $target=Join-Path $Destination $Relative
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
    Copy-Item -LiteralPath (Join-Path $taskRoot $Relative) -Destination $target
}
function Assert-PublicReleaseBinary([string]$Path){
    $bytes=[IO.File]::ReadAllBytes($Path)
    $texts=@([Text.Encoding]::UTF8.GetString($bytes),[Text.Encoding]::Unicode.GetString($bytes))
    if($bytes.Length -gt 1){$texts+=([Text.Encoding]::Unicode.GetString($bytes,1,$bytes.Length-1))}
    foreach($text in $texts){
        if($text -match '(?i)\b[a-z]:[\\/]Users[\\/](?!Public\b|Default\b)[^\s\\/`"<>]+' -or $text -match '\bS-1-5-21-\d+-\d+-\d+-\d+\b'){
            throw ('Personal path/SID in executable: '+[IO.Path]::GetFileName($Path)+'. Rebuild the release targets with NDEBUG; do not publish this binary.')
        }
    }
}
$taskRuntimeFiles=@('LICENSE','third_party/json/LICENSE.MIT','third_party/vigem/LICENSE',
    'docs/INSTALL.md','docs/TROUBLESHOOTING.md','docs/cpp-bridge.md',
    'docs/LIMITATIONS.md','docs/PRIVACY.md','docs/UPSTREAM.md',
    'scripts/Setup-Action.ps1','scripts/Setup-Helpers.ps1','scripts/Device-Config.ps1',
    'scripts/Switch-WinUSB.ps1','scripts/Adapter-IdlePower.ps1','scripts/Set-AdapterIdlePower.ps1','scripts/Import-Firmware.ps1',
    'scripts/Set-AutomaticBridge.ps1','scripts/Run-MaintainedBridge.ps1','scripts/Get-ControllerStatus.ps1','docs/RUNTIME-IO.md')
$taskSourceExtra=@('README.md','.gitignore','.gitattributes','CHANGELOG.md',
    '.github/workflows/public-content.yml')
Push-Location -LiteralPath $taskRoot
try{
    # Include new, non-ignored source files as well as tracked files. Never
    # derive the installed payload from the complete development checkout.
    $taskFiles=@(& git -c core.quotePath=false ls-files -co --exclude-standard | Sort-Object -Unique)
    if($LASTEXITCODE -ne 0 -or -not $taskFiles.Count){throw 'Package from a Git source checkout.'}
    foreach($taskFile in $taskFiles){
        if($taskFile -match '^(work|outputs|build|dist|runtime|\.git|\.agents|\.codex)/' -or $taskFile -match '\.local\.' -or $taskFile -match '(^|/)(__pycache__|\.pytest_cache)/' -or $taskFile -match '\.(pyc|log|bin|pdb)$'){continue}
        if($taskFile -match '^(src|tests|driver|third_party|installer|config|scripts|docs)/' -or $taskFile -in $taskRuntimeFiles -or $taskFile -in $taskSourceExtra){
            Copy-PackageFile $taskFile $taskSourceStage
        }
    }
    foreach($taskFile in $taskRuntimeFiles){Copy-PackageFile $taskFile $taskStage}
    $taskOutput=Join-Path $taskStage 'outputs'
    [void][IO.Directory]::CreateDirectory($taskOutput)
    foreach($taskBinary in @('xone-probe-cpp.exe','xone-background.exe','xone-bind.exe')){
        $taskSource=Join-Path $taskRoot ('outputs/'+$taskBinary)
        if($LauncherDir -and $taskBinary -in @('xone-probe-cpp.exe','xone-background.exe')){$taskSource=Join-Path $LauncherDir $taskBinary}
        if(-not (Test-Path -LiteralPath $taskSource)){throw ('Missing release binary: '+$taskBinary)}
        Assert-PublicReleaseBinary $taskSource
        Copy-Item -LiteralPath $taskSource -Destination $taskOutput
    }
    if($LauncherDir -and (Test-Path -LiteralPath (Join-Path $LauncherDir 'runtime-capabilities.json'))){Copy-Item -LiteralPath (Join-Path $LauncherDir 'runtime-capabilities.json') -Destination $taskOutput}
    $taskCommit=(& git rev-parse HEAD).Trim()
    $taskDirty=[bool]@(& git status --porcelain).Count
    foreach($taskPackage in @($taskSourceStage,$taskStage)){
        $taskManifest=[ordered]@{Project='xone-windows-wireless';Version=$Version;Architecture='x64';BaseCommit=$taskCommit;WorkingTreeModified=$taskDirty;BuiltAt=[datetime]::UtcNow.ToString('o');FirmwareIncluded=$false;MicrosoftDriversIncluded=$false;PrivateConfigurationIncluded=$false;Files=@()}
        foreach($taskFile in Get-ChildItem -LiteralPath $taskPackage -File -Recurse | Sort-Object FullName){
            $taskRelative=$taskFile.FullName.Substring($taskPackage.Length+1).Replace('\','/')
            $taskManifest.Files+=@{Path=$taskRelative;SHA256=(Get-FileHash -LiteralPath $taskFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant();Bytes=$taskFile.Length}
        }
        [IO.File]::WriteAllText((Join-Path $taskPackage 'release-manifest.json'),($taskManifest | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
        $taskArchive=$taskPackage+'.zip'
        Compress-Archive -LiteralPath $taskPackage -DestinationPath $taskArchive -CompressionLevel Optimal
        Write-Output $taskArchive
    }
}finally{Pop-Location}
