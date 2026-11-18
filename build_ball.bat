@echo off
rem Portable build script for ball.exe
rem Toolchain: MSYS2 UCRT64 (g++.exe / windres.exe must be in UCRT64_BIN)
rem Usage:
rem   set MSYS2_UCRT64_BIN=C:\msys64\ucrt64\bin
rem   build_ball.bat
setlocal
set ROOT=%~dp0
set SRC=%ROOT%src
set OUT=%ROOT%bin
set UCRT64_BIN=C:\msys64\ucrt64\bin
if defined MSYS2_UCRT64_BIN set UCRT64_BIN=%MSYS2_UCRT64_BIN%

if not exist "%UCRT64_BIN%\g++.exe" (
    echo ERROR: g++.exe not found in %UCRT64_BIN%
    echo Set MSYS2_UCRT64_BIN to your MSYS2 ucrt64\bin directory.
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%UCRT64_BIN%;%PATH%

cd /d "%SRC%"
windres.exe -i ball.rc -o ball_res.o
if errorlevel 1 (
    echo WINDRES FAILED
    exit /b 1
)

rem -pipe is required here: MSYS2 binutils as.exe may segfault on temp files.
g++.exe -pipe -Os -municode -mwindows -fno-exceptions -fno-rtti -c -o ball.o ball.cpp
if errorlevel 1 (
    echo COMPILE FAILED
    exit /b 1
)

rem Static link: no MinGW/C++ runtime DLLs. --no-insert-timestamp keeps PE stable.
g++.exe -municode -mwindows -s -static -static-libgcc -static-libstdc++ ^
    -Wl,--no-insert-timestamp ^
    -o "%OUT%\ball.exe" ball.o ball_res.o ^
    -lgdi32 -luser32 -lshell32 -lole32 -lpdh -lcomctl32 -liphlpapi -ladvapi32
if errorlevel 1 (
    echo LINK FAILED
    exit /b 1
)

echo BUILD OK: %OUT%\ball.exe
endlocal
