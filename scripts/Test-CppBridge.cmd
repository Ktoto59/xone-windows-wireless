@echo off
setlocal
pushd "%~dp0.."
if not exist build\usbip_native_output.obj (
    echo Build-CppBridge.cmd must complete first.
    exit /b 1
)
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "XONE_VS=%%i"
)
if not defined XONE_VS exit /b 1
call "%XONE_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /I src /c tests\usbip_native_codec_tests.cpp tests\usbip_native_core_tests.cpp tests\usbip_native_server_probe.cpp tests\native_multi_route_probe.cpp tests\latest_snapshot_tests.cpp /Fobuild\
if errorlevel 1 exit /b 1
link /nologo build\usbip_native_codec_tests.obj build\usbip_native_codec.obj build\protocol.obj /out:outputs\usbip-native-codec-tests.exe
if errorlevel 1 exit /b 1
link /nologo build\usbip_native_core_tests.obj build\usbip_native_core.obj build\usbip_native_codec.obj build\gip_link.obj build\protocol.obj /out:outputs\usbip-native-core-tests.exe
if errorlevel 1 exit /b 1
link /nologo build\usbip_native_server_probe.obj build\usbip_native_output.obj build\usbip_native_core.obj build\usbip_native_codec.obj build\gip_link.obj build\protocol.obj ws2_32.lib avrt.lib /out:outputs\usbip-native-server-probe.exe
if errorlevel 1 exit /b 1
link /nologo build\latest_snapshot_tests.obj /out:outputs\latest-snapshot-tests.exe
if errorlevel 1 exit /b 1
link /nologo build\native_multi_route_probe.obj build\usbip_native_output.obj build\usbip_native_core.obj build\usbip_native_codec.obj build\gip_link.obj build\protocol.obj ws2_32.lib avrt.lib /out:outputs\native-multi-route-probe.exe
if errorlevel 1 exit /b 1
outputs\usbip-native-codec-tests.exe
if errorlevel 1 exit /b 1
outputs\usbip-native-core-tests.exe
if errorlevel 1 exit /b 1
outputs\latest-snapshot-tests.exe
if errorlevel 1 exit /b 1
python -m unittest discover -s tests -p usbip_native_*tests.py
if errorlevel 1 exit /b 1
popd
endlocal
