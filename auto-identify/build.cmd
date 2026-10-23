@echo off
rem Builds d2rl-auto-identify.dll with MSVC x64 + Ninja (Visual Studio at M:\Tools\Visual Studio).
setlocal
call "M:\Tools\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=M:\Tools\Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;M:\Tools\Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
endlocal
