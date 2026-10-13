@echo off
setlocal
pushd "%~dp0.."
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "XONE_VS=%%i"
)
if not defined XONE_VS (
    echo Visual Studio C++ Build Tools not found.
    exit /b 1
)
call "%XONE_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist build mkdir build
if not exist outputs mkdir outputs
cl /nologo /TC /std:c11 /O2 /W3 /wd4244 /wd4267 /D_CRT_SECURE_NO_WARNINGS /c src\mt76\mt76.c /Fobuild\mt76.obj
if errorlevel 1 exit /b 1
cl /nologo /TC /std:c11 /O2 /W4 /WX /c src\platform\xone_compat.c /Fobuild\xone_compat.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /c src\protocol.cpp /Fobuild\protocol.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /c src\gip_link.cpp /Fobuild\gip_link.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /c src\auth.cpp /Fobuild\auth.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /c src\session.cpp /Fobuild\session.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /c src\raw_gip.cpp /Fobuild\raw_gip.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /c src\input_mapping.cpp /Fobuild\input_mapping.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /I third_party\vigem\include /c src\vigem_output.cpp /Fobuild\vigem_output.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /c src\native_gip_output.cpp /Fobuild\native_gip_output.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /c src\native_audio_output.cpp /Fobuild\native_audio_output.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /c src\winusb_transport.cpp /Fobuild\winusb_transport.obj
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /c src\main.cpp /Fobuild\main.obj
if errorlevel 1 exit /b 1
link /nologo build\main.obj build\winusb_transport.obj build\protocol.obj build\gip_link.obj build\auth.obj build\session.obj build\raw_gip.obj build\input_mapping.obj build\vigem_output.obj build\native_gip_output.obj build\native_audio_output.obj build\mt76.obj build\xone_compat.obj setupapi.lib winusb.lib xinput.lib bcrypt.lib uuid.lib runtimeobject.lib ws2_32.lib /out:outputs\xone-probe.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 src\native_gip_probe.cpp build\native_gip_output.obj build\input_mapping.obj /Fobuild\native_gip_probe.obj /Feoutputs\native-gip-probe.exe /link xinput.lib runtimeobject.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 src\native_audio_probe.cpp /Fobuild\native_audio_probe.obj /Feoutputs\native-audio-probe.exe /link ole32.lib uuid.lib propsys.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 src\audio_endpoint_test.cpp /Fobuild\audio_endpoint_test.obj /Feoutputs\audio-endpoint-test.exe /link ole32.lib uuid.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /c src\driver_binding.cpp /Fobuild\driver_binding.obj
if errorlevel 1 exit /b 1
link /nologo build\driver_binding.obj build\winusb_transport.obj setupapi.lib winusb.lib newdev.lib advapi32.lib uuid.lib /out:outputs\xone-bind.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /I src /c tests\protocol_tests.cpp /Fobuild\protocol_tests.obj
if errorlevel 1 exit /b 1
link /nologo build\protocol_tests.obj build\protocol.obj build\mt76.obj build\xone_compat.obj /out:outputs\protocol-tests.exe
if errorlevel 1 exit /b 1
outputs\protocol-tests.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /utf-8 /I src /c tests\session_tests.cpp /Fobuild\session_tests.obj
if errorlevel 1 exit /b 1
link /nologo build\session_tests.obj build\protocol.obj build\gip_link.obj build\auth.obj build\session.obj build\input_mapping.obj build\mt76.obj build\xone_compat.obj bcrypt.lib /out:outputs\session-tests.exe
if errorlevel 1 exit /b 1
outputs\session-tests.exe
if errorlevel 1 exit /b 1
popd
endlocal
