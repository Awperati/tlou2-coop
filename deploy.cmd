@echo off
rem Copies the build into the game. Safe while the game runs: coop.dll hot-reloads.
rem The mod is attached by tlou2-coop\coop_launcher.exe. A version.dll in the game folder
rem would load it with the game instead: set COOP_AUTOLOAD=1 to keep that way.
setlocal
set GAME=E:\Games\The Last of Us - Part II Remastered
if not "%~1"=="" set GAME=%~1
if not exist "%GAME%\tlou2-coop" mkdir "%GAME%\tlou2-coop"
if "%COOP_AUTOLOAD%"=="1" (
  fc /b "build\version.dll" "%GAME%\version.dll" >nul 2>&1 || (
    del "%GAME%\version.old.dll" >nul 2>&1
    copy /y "build\version.dll" "%GAME%\version.dll" >nul 2>&1 || (
      ren "%GAME%\version.dll" version.old.dll && copy /y "build\version.dll" "%GAME%\version.dll" >nul
    )
  )
) else if exist "%GAME%\version.dll" (
  rem A loaded version.dll can't be deleted, but it can be renamed.
  del "%GAME%\version.disabled.dll" >nul 2>&1
  ren "%GAME%\version.dll" version.disabled.dll
)
for %%f in (coop.ini roster.ini) do if not exist "%GAME%\tlou2-coop\%%f" copy /y "config\%%f" "%GAME%\tlou2-coop\" >nul
copy /y "config\offsets.ini" "%GAME%\tlou2-coop\" >nul
copy /y "config\looks.ini" "%GAME%\tlou2-coop\" >nul
copy /y "build\coop_inject.dll" "%GAME%\tlou2-coop\" >nul 2>&1
copy /y "build\coop_launcher.exe" "%GAME%\tlou2-coop\" >nul 2>&1
copy /y "build\coop.dll" "%GAME%\tlou2-coop\coop.dll" >nul
rem The partner's mod fetches this copy through tools\share.py ([session] AutoUpdateFromHost).
if exist "share" copy /y "build\coop.dll" "share\coop.dll" >nul
echo Deployed to %GAME%
