@echo off
setlocal
pushd "%~dp0.."
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "XONE_VS=%%i"
if not defined XONE_VS exit /b 1
call "%XONE_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist outputs mkdir outputs
if not exist build mkdir build
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 src\etw_trace_dump.cpp /Fobuild\etw_trace_dump.obj /Feoutputs\etw-trace-dump.exe /link advapi32.lib
if errorlevel 1 exit /b 1
popd
endlocal
