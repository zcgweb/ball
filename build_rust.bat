@echo off
setlocal
cd /d "%~dp0"

where cargo >nul 2>nul
if errorlevel 1 (
  echo [ERROR] cargo not found in PATH. Install Rust stable first.
  exit /b 1
)

set "UCRT64_BIN=C:\msys64\ucrt64\bin"
if defined MSYS2_UCRT64_BIN set "UCRT64_BIN=%MSYS2_UCRT64_BIN%"
if not exist "%UCRT64_BIN%\windres.exe" (
  echo [ERROR] windres.exe not found in %UCRT64_BIN%
  echo Set MSYS2_UCRT64_BIN to your MSYS2 ucrt64\bin directory.
  exit /b 1
)

if not exist "src\ball.rc" (
  echo [ERROR] src\ball.rc not found.
  exit /b 1
)
if not exist "src\ball.ico" (
  echo [ERROR] src\ball.ico not found.
  exit /b 1
)
if not exist "target" mkdir target

rem Compile the icon resource from src\ball.rc (ball.ico is resolved relative to src\).
pushd src
"%UCRT64_BIN%\windres.exe" -i ball.rc -o "..\target\ball_res.o"
popd
if errorlevel 1 (
  echo [ERROR] windres failed to compile src\ball.rc.
  exit /b 1
)

rem Link the COFF resource object into the Rust release binary.
set "RES_OBJ=%CD:\=/%/target/ball_res.o"
set "RUSTFLAGS=%RUSTFLAGS% -C link-arg=%RES_OBJ%"

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

echo [OK] Built bin\ball.exe from src\main.rs with src\ball.ico embedded.
endlocal
