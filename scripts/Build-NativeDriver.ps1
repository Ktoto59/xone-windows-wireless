# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$WdkRoot,
      [string]$SdkRoot="${env:ProgramFiles(x86)}\Windows Kits\10",
      [ValidatePattern('^10\.0\.\d+\.0$')][string]$KitVersion='10.0.26100.0')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskKernel=Join-Path $WdkRoot ('c/Include/'+$KitVersion+'/km')
$taskLibraries=Join-Path $WdkRoot ('c/Lib/'+$KitVersion+'/km/x64')
if(-not (Test-Path -LiteralPath (Join-Path $taskKernel 'ndis.h'))){throw 'Extract the matching x64 WDK NuGet package and pass its root.'}
$taskVswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVs=(& $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if(-not $taskVs){throw 'Visual Studio C++ x64 Build Tools are required.'}
$taskBuild=Join-Path $taskRoot 'build/native'
$taskOutput=Join-Path $taskRoot 'outputs/native'
[void][IO.Directory]::CreateDirectory($taskBuild)
[void][IO.Directory]::CreateDirectory($taskOutput)
$taskEnvironment=Join-Path $taskVs 'Common7/Tools/VsDevCmd.bat'
$taskShared=Join-Path $SdkRoot ('Include/'+$KitVersion+'/shared')
$taskCrt=Join-Path $taskKernel 'crt'
$taskFlags='/nologo /TC /std:c11 /kernel /O2 /W4 /WX /wd4324 /utf-8 /GS /DXONE_KERNEL /D_AMD64_ /DAMD64 /D_WIN64 /D_NDIS_MINIPORT_DRIVER_ /DNDIS630_MINIPORT /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS /I"'+$taskKernel+'" /I"'+$taskShared+'" /I"'+$taskCrt+'"'
$taskLines=@('@echo off','setlocal',('call "'+$taskEnvironment+'" -arch=x64 -host_arch=x64 >nul'),'if errorlevel 1 exit /b 1')
foreach($taskUnit in @('miniport','usb_transport','kernel_compat')){
    $taskLines+='cl '+$taskFlags+' /c "'+(Join-Path $taskRoot ('driver/native/'+$taskUnit+'.c'))+'" /Fo"'+(Join-Path $taskBuild ($taskUnit+'.obj'))+'"'
    $taskLines+='if errorlevel 1 exit /b 1'
}
# Upstream C radio code has the same known integer conversions as the user-mode build.
$taskLines+='cl '+($taskFlags.Replace('/W4 /WX','/W3 /wd4244 /wd4267'))+' /c "'+(Join-Path $taskRoot 'src/mt76/mt76.c')+'" /Fo"'+(Join-Path $taskBuild 'mt76.obj')+'"'
$taskLines+='if errorlevel 1 exit /b 1'
$taskObjects=(@('miniport','usb_transport','kernel_compat','mt76') | ForEach-Object {'"'+(Join-Path $taskBuild ($_+'.obj'))+'"'}) -join ' '
$taskLines+='link /nologo /driver /subsystem:native /entry:GsDriverEntry /machine:x64 /release /incremental:no /nodefaultlib '+$taskObjects+' /libpath:"'+$taskLibraries+'" ntoskrnl.lib hal.lib ndis.lib usbd.lib bufferoverflowfastfailk.lib /out:"'+(Join-Path $taskOutput 'xone-native.sys')+'"'
$taskLines+='if errorlevel 1 exit /b 1'
$taskLines+='endlocal'
$taskCommand=Join-Path $taskBuild 'compile.cmd'
[IO.File]::WriteAllLines($taskCommand,$taskLines,[Text.Encoding]::Default)
& $env:ComSpec /d /c ('"'+$taskCommand+'"')
if($LASTEXITCODE -ne 0){throw 'Native driver compilation failed.'}
Copy-Item -LiteralPath (Join-Path $taskRoot 'driver/native/xone-native.inf') -Destination $taskOutput
Write-Output ('Native bootstrap built: '+(Join-Path $taskOutput 'xone-native.sys'))
Write-Output 'Unsigned development artifact; GIP data plane is incomplete. No driver installation or tests performed.'
