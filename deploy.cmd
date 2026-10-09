@echo off
rem Copies the build into the game. Safe while the game runs:
rem coop.dll hot-reloads; a locked version.dll is renamed aside and replaced.
setlocal
set GAME=E:\Games\The Last of Us - Part II Remastered
if not "%~1"=="" set GAME=%~1
if not exist "%GAME%\tlou2-coop" mkdir "%GAME%\tlou2-coop"
fc /b "build\version.dll" "%GAME%\version.dll" >nul 2>&1 || (
  del "%GAME%\version.old.dll" >nul 2>&1
  copy /y "build\version.dll" "%GAME%\version.dll" >nul 2>&1 || (
    ren "%GAME%\version.dll" version.old.dll && copy /y "build\version.dll" "%GAME%\version.dll" >nul
  )
)
for %%f in (coop.ini roster.ini) do if not exist "%GAME%\tlou2-coop\%%f" copy /y "config\%%f" "%GAME%\tlou2-coop\" >nul
copy /y "config\offsets.ini" "%GAME%\tlou2-coop\" >nul
copy /y "build\coop.dll" "%GAME%\tlou2-coop\coop.dll" >nul
echo Deployed to %GAME%
