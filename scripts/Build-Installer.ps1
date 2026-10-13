# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$IsccPath,
      [ValidatePattern('^\d+\.\d+\.\d+(?:-[a-z0-9.]+)?$')][string]$Version='0.1.0-alpha.4')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskTools=Join-Path $taskRoot 'work/installer-tools'
[void][IO.Directory]::CreateDirectory($taskTools)
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
$taskPythonArchive=Join-Path $taskTools 'python-3.13.16-embed-amd64.zip'
Get-XoneDownload 'https://www.python.org/ftp/python/3.13.16/python-3.13.16-embed-amd64.zip' $taskPythonArchive '97dae5274cc54867065e8d5a3226e48c35017ed332a0fdb0e27d5b5821961297'
$taskPythonDir=Join-Path $taskTools ('python-'+$Version)
if(Test-Path -LiteralPath $taskPythonDir){throw 'Runtime staging already exists. Use a fresh version or explicitly remove the previous stage.'}
Expand-Archive -LiteralPath $taskPythonArchive -DestinationPath $taskPythonDir
# Keep Python isolated from system Python and user packages. Only shipped code is added.
[IO.File]::WriteAllText((Join-Path $taskPythonDir 'python313._pth'),"python313.zip`n.`n..\..\src`n..\..`n",[Text.UTF8Encoding]::new($false))
$taskCompiler=Join-Path $env:windir 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$taskLauncherDir=Join-Path $taskTools ('launchers-'+$Version)
[void][IO.Directory]::CreateDirectory($taskLauncherDir)
# Build outside outputs: a running background host must never be overwritten.
& $taskCompiler /nologo /target:winexe /platform:x64 /optimize+ /reference:System.Windows.Forms.dll ('/out:'+(Join-Path $taskLauncherDir 'xone-control.exe')) (Join-Path $taskRoot 'src/control_launcher.cs')
if($LASTEXITCODE -ne 0){throw 'Control launcher compilation failed.'}
& $taskCompiler /nologo /target:winexe /platform:x64 /optimize+ ('/out:'+(Join-Path $taskLauncherDir 'xone-background.exe')) (Join-Path $taskRoot 'src/background_launcher.cs')
if($LASTEXITCODE -ne 0){throw 'Background launcher compilation failed.'}
& (Join-Path $PSScriptRoot 'Package-Release.ps1') -Version $Version -LauncherDir $taskLauncherDir
$taskStage=Join-Path $taskRoot ('dist/xone-windows-wireless-'+$Version+'-windows-x64')
& $IsccPath ('/DAppVersion='+$Version) ('/DStageDir='+$taskStage) ('/DPythonDir='+$taskPythonDir) (Join-Path $taskRoot 'installer/xone-windows-wireless.iss')
if($LASTEXITCODE -ne 0){throw 'Inno Setup compilation failed.'}
Write-Output (Join-Path $taskRoot ('dist/xone-windows-wireless-'+$Version+'-Setup-x64.exe'))
