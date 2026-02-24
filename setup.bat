@echo off
setlocal

set "TOOLS_DIR=%~dp0tools"
set "ZIG_DIR=%TOOLS_DIR%\zig"

if exist "%ZIG_DIR%\zig.exe" (
    echo Zig already installed at %ZIG_DIR%
    exit /b 0
)

echo Downloading Zig 0.15.2...
mkdir "%TOOLS_DIR%" 2>NUL
curl -L -o "%TOOLS_DIR%\zig.zip" "https://ziglang.org/download/0.15.2/zig-x86_64-windows-0.15.2.zip"
if %ERRORLEVEL% neq 0 (
    echo Download failed.
    exit /b 1
)

echo Extracting Zig...
powershell -Command "Expand-Archive -Force '%TOOLS_DIR%\zig.zip' '%TOOLS_DIR%'"
del "%TOOLS_DIR%\zig.zip"

:: The zip extracts to zig-x86_64-windows-0.15.2/, rename it to zig/
if exist "%TOOLS_DIR%\zig-x86_64-windows-0.15.2" (
    ren "%TOOLS_DIR%\zig-x86_64-windows-0.15.2" zig
)

if exist "%ZIG_DIR%\zig.exe" (
    echo Zig installed successfully.
) else (
    echo Zig extraction failed.
    exit /b 1
)

endlocal
