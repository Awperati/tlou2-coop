@echo off
rem Builds a self-contained folder for the first-person mod, outside the game folder:
rem   tools\fp_launcher\package.cmd "<folder with TLOU2FirstPerson.asi>" ["<output folder>"]
rem Default output: %USERPROFILE%\TLOU2 FirstPerson. The ASI loader (winmm.dll) is NOT copied: fp_launcher.exe loads
rem the ASI itself. Build first (build.cmd, or cmake --build build --target fp_launcher).
setlocal
if "%~1"=="" ( echo usage: package.cmd "<folder with TLOU2FirstPerson.asi>" ["<output folder>"] & exit /b 2 )
set SRC=%~1
set OUT=%~2
if "%OUT%"=="" set OUT=%USERPROFILE%\TLOU2 FirstPerson
if not exist "%SRC%\TLOU2FirstPerson.asi" ( echo no TLOU2FirstPerson.asi in "%SRC%" & exit /b 1 )
if not exist "build\fp_launcher.exe" ( echo build\fp_launcher.exe missing: build it first & exit /b 1 )
if not exist "%OUT%" mkdir "%OUT%"
copy /y "build\fp_launcher.exe" "%OUT%\" >nul
copy /y "%SRC%\TLOU2FirstPerson.asi" "%OUT%\" >nul
if exist "%SRC%\TLOU2FirstPerson.ini" if not exist "%OUT%\TLOU2FirstPerson.ini" copy /y "%SRC%\TLOU2FirstPerson.ini" "%OUT%\" >nul
copy /y "tools\fp_launcher\README.txt" "%OUT%\" >nul
echo Packaged to %OUT%
