@echo off
setlocal
cd /d "%~dp0"

where cargo >nul 2>nul
if errorlevel 1 (
  echo [ERROR] cargo not found in PATH. Install Rust stable first.
  exit /b 1
)

cargo build --release
if errorlevel 1 (
  echo [ERROR] cargo build --release failed.
  exit /b 1
)

if not exist "target\release\ball.exe" (
  echo [ERROR] target\release\ball.exe not found after build.
  exit /b 1
)

if not exist "bin" mkdir bin
copy /Y "target\release\ball.exe" "bin\ball.exe" >nul
if errorlevel 1 (
  echo [ERROR] failed to copy release binary to bin\ball.exe.
  exit /b 1
)

echo [OK] Built bin\ball.exe from src\main.rs
endlocal
