@echo off
rem Builds Release into .\build. Needs Visual Studio 2022 Build Tools (C++).
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set CTEST=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe
"%CMAKE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
"%CMAKE%" --build build || exit /b 1
"%CTEST%" --test-dir build --output-on-failure
