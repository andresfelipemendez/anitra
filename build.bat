@echo off
setlocal

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "ZIG=%ROOT%\tools\zig\zig.exe"

if not exist "%ZIG%" (
    echo Zig not found. Run setup.bat first.
    exit /b 1
)

echo === Building all targets ===
"%ZIG%" build --prefix "%ROOT%\build\Debug"
if %ERRORLEVEL% neq 0 (
    echo Build failed.
    exit /b 1
)

echo.
echo Build completed successfully.
echo Output: %ROOT%\build\Debug\

endlocal
