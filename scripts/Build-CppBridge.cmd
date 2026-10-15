@echo off
setlocal
pushd "%~dp0.."
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "XONE_VS=%%i"
)
if not defined XONE_VS exit /b 1
call "%XONE_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist build mkdir build
if not exist outputs mkdir outputs
cl /nologo /TC /std:c11 /O2 /W3 /wd4244 /wd4267 /D_CRT_SECURE_NO_WARNINGS /c src\mt76\mt76.c /Fobuild\mt76.obj
if errorlevel 1 exit /b 1
cl /nologo /TC /std:c11 /O2 /W4 /WX /c src\platform\xone_compat.c /Fobuild\xone_compat.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /DNDEBUG /W4 /WX /utf-8 /D XONE_NATIVE_USB /I third_party\vigem\include /c src\winusb_transport.cpp src\protocol.cpp src\gip_link.cpp src\auth.cpp src\session.cpp src\raw_gip.cpp src\input_mapping.cpp src\vigem_output.cpp src\native_gip_output.cpp src\native_audio_output.cpp src\usbip_native_codec.cpp src\usbip_native_core.cpp src\usbip_native_output.cpp src\main.cpp /Fobuild\
if errorlevel 1 exit /b 1
link /nologo build\main.obj build\winusb_transport.obj build\protocol.obj build\gip_link.obj build\auth.obj build\session.obj build\raw_gip.obj build\input_mapping.obj build\vigem_output.obj build\native_gip_output.obj build\native_audio_output.obj build\usbip_native_codec.obj build\usbip_native_core.obj build\usbip_native_output.obj build\mt76.obj build\xone_compat.obj setupapi.lib winusb.lib xinput.lib bcrypt.lib uuid.lib runtimeobject.lib ws2_32.lib avrt.lib /out:outputs\xone-probe-cpp.exe
exit /b %errorlevel%
