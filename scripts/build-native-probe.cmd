@echo off
setlocal
pushd "%~dp0.."
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "XONE_VS=%%i"
if not defined XONE_VS exit /b 1
call "%XONE_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if not exist outputs mkdir outputs
if not exist build mkdir build
cl /nologo /std:c++20 /O2 /EHsc /W4 /WX /utf-8 /c src\native_gip_output.cpp /Fobuild\native_gip_output.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 src\native_gip_probe.cpp build\native_gip_output.obj build\input_mapping.obj /Fobuild\native_gip_probe.obj /Feoutputs\native-gip-probe.exe /link xinput.lib runtimeobject.lib user32.lib /MAP:build\native-gip-probe.map
exit /b %errorlevel%
