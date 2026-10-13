@echo off
setlocal
pushd "%~dp0.."
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "XONE_AUDIO_VS=%%i"
if not defined XONE_AUDIO_VS exit /b 1
call "%XONE_AUDIO_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 src\audio_endpoint_test.cpp /Fobuild\audio_endpoint_test.obj /Feoutputs\audio-endpoint-test.exe /link ole32.lib uuid.lib
set "XONE_AUDIO_RESULT=%errorlevel%"
popd
exit /b %XONE_AUDIO_RESULT%
