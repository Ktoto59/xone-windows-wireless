@echo off
setlocal
pushd "%~dp0.."
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "XONE_DIRECT_VS=%%i"
if not defined XONE_DIRECT_VS exit /b 1
call "%XONE_DIRECT_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist build\direct-gip mkdir build\direct-gip
if not exist outputs\direct-gip mkdir outputs\direct-gip
cl /nologo /TC /std:c11 /O2 /W3 /wd4244 /wd4267 /D_CRT_SECURE_NO_WARNINGS /c src\mt76\mt76.c /Fobuild\direct-gip\mt76.obj
if errorlevel 1 exit /b 1
cl /nologo /TC /std:c11 /O2 /W4 /WX /c src\platform\xone_compat.c /Fobuild\direct-gip\xone_compat.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /I third_party\vigem\include /c src\winusb_transport.cpp src\protocol.cpp src\gip_link.cpp src\auth.cpp src\session.cpp src\raw_gip.cpp src\input_mapping.cpp src\vigem_output.cpp src\native_gip_output.cpp src\native_audio_output.cpp src\usbip_native_codec.cpp src\usbip_native_core.cpp src\usbip_native_output.cpp src\main.cpp /Fobuild\direct-gip\
if errorlevel 1 exit /b 1
link /nologo build\direct-gip\main.obj build\direct-gip\winusb_transport.obj build\direct-gip\protocol.obj build\direct-gip\gip_link.obj build\direct-gip\auth.obj build\direct-gip\session.obj build\direct-gip\raw_gip.obj build\direct-gip\input_mapping.obj build\direct-gip\vigem_output.obj build\direct-gip\native_gip_output.obj build\direct-gip\native_audio_output.obj build\direct-gip\usbip_native_codec.obj build\direct-gip\usbip_native_core.obj build\direct-gip\usbip_native_output.obj build\direct-gip\mt76.obj build\direct-gip\xone_compat.obj setupapi.lib winusb.lib advapi32.lib xinput.lib bcrypt.lib uuid.lib runtimeobject.lib ws2_32.lib avrt.lib /out:outputs\direct-gip\xone-native.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 src\native_gip_probe.cpp build\direct-gip\native_gip_output.obj build\direct-gip\input_mapping.obj /Fobuild\direct-gip\native_gip_probe.obj /Feoutputs\direct-gip\native-gip-probe.exe /link xinput.lib runtimeobject.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 src\native_audio_probe.cpp /Fobuild\direct-gip\native_audio_probe.obj /Feoutputs\direct-gip\native-audio-probe.exe /link ole32.lib uuid.lib propsys.lib avrt.lib runtimeobject.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /I src /c tests\session_tests.cpp /Fobuild\direct-gip\session_tests.obj
if errorlevel 1 exit /b 1
link /nologo build\direct-gip\session_tests.obj build\direct-gip\protocol.obj build\direct-gip\gip_link.obj build\direct-gip\auth.obj build\direct-gip\session.obj build\direct-gip\input_mapping.obj build\direct-gip\mt76.obj build\direct-gip\xone_compat.obj bcrypt.lib /out:outputs\direct-gip\session-tests.exe
if errorlevel 1 exit /b 1
outputs\direct-gip\session-tests.exe
exit /b %errorlevel%
