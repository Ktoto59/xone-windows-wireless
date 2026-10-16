@echo off
setlocal
pushd "%~dp0.."
set "XONE_APPLIANCE_OUTPUT=outputs\appliance-candidate"
set "XONE_APPLIANCE_DIAGNOSTICS="
if not "%~1"=="" if not "%~1"=="--diagnostics" if not "%~1"=="--no-diagnostics" exit /b 2
if "%~1"=="--no-diagnostics" set "XONE_APPLIANCE_DIAGNOSTICS=/DXONE_DISABLE_DIAGNOSTICS"
if not "%~2"=="" set "XONE_APPLIANCE_OUTPUT=%~2"
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "XONE_APPLIANCE_VS=%%i"
if not defined XONE_APPLIANCE_VS exit /b 1
call "%XONE_APPLIANCE_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist build\appliance mkdir build\appliance
if not exist "%XONE_APPLIANCE_OUTPUT%" mkdir "%XONE_APPLIANCE_OUTPUT%"
cl /nologo /TC /std:c11 /O2 /W3 /wd4244 /wd4267 /D_CRT_SECURE_NO_WARNINGS /c src\mt76\mt76.c /Fobuild\appliance\mt76.obj
if errorlevel 1 exit /b 1
cl /nologo /TC /std:c11 /O2 /W4 /WX /c src\platform\xone_compat.c /Fobuild\appliance\xone_compat.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /DNDEBUG /W4 /WX /utf-8 %XONE_APPLIANCE_DIAGNOSTICS% /D XONE_NATIVE_USB /I third_party\vigem\include /c src\winusb_transport.cpp src\protocol.cpp src\gip_link.cpp src\auth.cpp src\session.cpp src\raw_gip.cpp src\input_mapping.cpp src\vigem_output.cpp src\native_gip_output.cpp src\native_audio_output.cpp src\usbip_native_codec.cpp src\usbip_native_core.cpp src\usbip_native_output.cpp src\main.cpp /Fobuild\appliance\
if errorlevel 1 exit /b 1
link /nologo build\appliance\main.obj build\appliance\winusb_transport.obj build\appliance\protocol.obj build\appliance\gip_link.obj build\appliance\auth.obj build\appliance\session.obj build\appliance\raw_gip.obj build\appliance\input_mapping.obj build\appliance\vigem_output.obj build\appliance\native_gip_output.obj build\appliance\native_audio_output.obj build\appliance\usbip_native_codec.obj build\appliance\usbip_native_core.obj build\appliance\usbip_native_output.obj build\appliance\mt76.obj build\appliance\xone_compat.obj setupapi.lib winusb.lib advapi32.lib xinput.lib bcrypt.lib uuid.lib runtimeobject.lib ws2_32.lib avrt.lib /out:"%XONE_APPLIANCE_OUTPUT%\xone-probe-cpp.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /DNDEBUG %XONE_APPLIANCE_DIAGNOSTICS% src\background_host.cpp build\appliance\winusb_transport.obj /Fobuild\appliance\background_host.obj /Fe"%XONE_APPLIANCE_OUTPUT%\xone-background.exe" /link /subsystem:windows ole32.lib uuid.lib propsys.lib setupapi.lib winusb.lib cfgmgr32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /I src tests\session_tests.cpp build\appliance\protocol.obj build\appliance\gip_link.obj build\appliance\auth.obj build\appliance\session.obj build\appliance\input_mapping.obj build\appliance\mt76.obj build\appliance\xone_compat.obj /Fobuild\appliance\session_tests.obj /Fe"%XONE_APPLIANCE_OUTPUT%\session-tests.exe" /link bcrypt.lib
if errorlevel 1 exit /b 1
"%XONE_APPLIANCE_OUTPUT%\session-tests.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /I src tests\appliance_config_tests.cpp /Fobuild\appliance\appliance_config_tests.obj /Fe"%XONE_APPLIANCE_OUTPUT%\appliance-config-tests.exe"
if errorlevel 1 exit /b 1
"%XONE_APPLIANCE_OUTPUT%\appliance-config-tests.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /I src tests\adapter_catalog_tests.cpp /Fobuild\appliance\adapter_catalog_tests.obj /Fe"%XONE_APPLIANCE_OUTPUT%\adapter-catalog-tests.exe"
if errorlevel 1 exit /b 1
"%XONE_APPLIANCE_OUTPUT%\adapter-catalog-tests.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /I src tests\radio_restart_backoff_tests.cpp /Fobuild\appliance\radio_restart_backoff_tests.obj /Fe"%XONE_APPLIANCE_OUTPUT%\radio-restart-backoff-tests.exe"
if errorlevel 1 exit /b 1
"%XONE_APPLIANCE_OUTPUT%\radio-restart-backoff-tests.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /I src tests\radio_idle_power_tests.cpp /Fobuild\appliance\radio_idle_power_tests.obj /Fe"%XONE_APPLIANCE_OUTPUT%\radio-idle-power-tests.exe"
if errorlevel 1 exit /b 1
"%XONE_APPLIANCE_OUTPUT%\radio-idle-power-tests.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /I src tests\control_media_queue_tests.cpp build\appliance\protocol.obj build\appliance\gip_link.obj /Fobuild\appliance\control_media_queue_tests.obj /Fe"%XONE_APPLIANCE_OUTPUT%\control-media-queue-tests.exe"
if errorlevel 1 exit /b 1
"%XONE_APPLIANCE_OUTPUT%\control-media-queue-tests.exe"
if errorlevel 1 exit /b 1
if defined XONE_APPLIANCE_DIAGNOSTICS (
    cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /DNDEBUG /DXONE_DISABLE_DIAGNOSTICS /I src tests\diagnostics_disabled_tests.cpp /Fobuild\appliance\diagnostics_disabled_tests.obj /Fe"%XONE_APPLIANCE_OUTPUT%\diagnostics-disabled-tests.exe"
    if errorlevel 1 exit /b 1
    "%XONE_APPLIANCE_OUTPUT%\diagnostics-disabled-tests.exe" work\disabled-journal-test-%RANDOM%-%RANDOM%
    if errorlevel 1 exit /b 1
)
exit /b 0
