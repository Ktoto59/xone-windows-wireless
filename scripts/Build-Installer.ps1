# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$IsccPath,
      [ValidatePattern('^\d+\.\d+\.\d+(?:-[a-z0-9.]+)?$')][string]$Version='0.1.0-alpha.5.4',[switch]$SkipBuild,[string]$LauncherDir='',
      [switch]$EnableDiagnostics)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskTools=Join-Path $taskRoot 'work/installer-tools'
[void][IO.Directory]::CreateDirectory($taskTools)
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
$taskLauncherDir=if($LauncherDir){[IO.Path]::GetFullPath($LauncherDir)}elseif($EnableDiagnostics){Join-Path $taskRoot 'outputs/appliance-candidate'}else{Join-Path $taskRoot 'outputs/appliance-release'}
if(-not $SkipBuild){
    $taskBuildMode=if($EnableDiagnostics){'--diagnostics'}else{'--no-diagnostics'}
    & (Join-Path $PSScriptRoot 'Build-Appliance.cmd') $taskBuildMode $taskLauncherDir
    if($LASTEXITCODE -ne 0){throw 'C++ background build failed.'}
}
& (Join-Path $PSScriptRoot 'Package-Release.ps1') -Version $Version -LauncherDir $taskLauncherDir
$taskStage=Join-Path $taskRoot ('dist/xone-windows-wireless-'+$Version+'-windows-x64')
& $IsccPath ('/DAppVersion='+$Version) ('/DStageDir='+$taskStage) (Join-Path $taskRoot 'installer/xone-windows-wireless.iss')
if($LASTEXITCODE -ne 0){throw 'Inno Setup compilation failed.'}
Write-Output (Join-Path $taskRoot ('dist/xone-windows-wireless-'+$Version+'-Setup-x64.exe'))
