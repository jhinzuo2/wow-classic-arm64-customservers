@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

rem ---------------------------------------------------------------------------
rem  Builds the Arctium-style Classic Era launcher as a 64-bit Windows exe 
rem  with MinGW-w64 (e.g. MSYS2 "mingw64"). Output goes to the build\ folder.
rem ---------------------------------------------------------------------------

set "CC="
where x86_64-w64-mingw32-gcc >nul 2>nul && set "CC=x86_64-w64-mingw32-gcc"
if not defined CC (
    where gcc >nul 2>nul && set "CC=gcc"
)
if not defined CC (
    echo [error] No MinGW-w64 gcc found in PATH.
    echo         Install MSYS2 ^(https://www.msys2.org^) and run:  pacman -S mingw-w64-x86_64-gcc
    echo         then add C:\msys64\mingw64\bin to PATH, or start this from the "MSYS2 MinGW x64" shell.
    exit /b 1
)

rem Make sure the compiler really produces 64-bit code.
for /f "delims=" %%m in ('%CC% -dumpmachine') do set "MACHINE=%%m"
echo Compiler: %CC%  ^(%MACHINE%^)
echo !MACHINE! | findstr /i "x86_64" >nul
if errorlevel 1 (
    echo [error] This gcc targets "%MACHINE%", not x86_64. Use the mingw64 toolchain.
    exit /b 1
)

set "CFLAGS=-O2 -Wall -Wno-unused-function -static"
if not exist build mkdir build
set "FAIL=0"

call :build ArctiumClassicEra.c    ArctiumClassicEra.exe    -lversion

echo.
if "!FAIL!"=="1" (
    echo Build FAILED.
    exit /b 1
)
echo Done. Binary is in %~dp0build\
exit /b 0

:build
rem %1 = source, %2 = output name, %3 = extra libs
if not exist "%~1" (
    echo [skip]  %~1 not found
    exit /b 0
)
echo [build] %~1 -> build\%~2
%CC% %CFLAGS% -o "build\%~2" "%~1" %~3
if errorlevel 1 (
    echo [FAIL]  %~1
    set "FAIL=1"
) else (
    echo [ok]    build\%~2
)
exit /b 0