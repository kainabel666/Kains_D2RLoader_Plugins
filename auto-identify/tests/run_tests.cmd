@echo off
rem Builds and runs the config parser test (MSVC x64).
setlocal
call "M:\Tools\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /std:c++20 /EHsc /W4 /permissive- /I "..\..\third_party\PluginSDK\include" /Fo"out\\" /Fe"out\parse_test.exe" parse_test.cpp || exit /b 1
out\parse_test.exe
endlocal
