@echo off
setlocal

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "ZIG=%ROOT%\tools\zig\zig.exe"

if not exist "%ZIG%" (
    echo Zig not found. Run setup.bat first.
    exit /b 1
)

"%ZIG%" build -Donly=engine --prefix "%ROOT%\build\Debug"
if %ERRORLEVEL% neq 0 (
    echo Engine build failed.
    exit /b 1
)

echo Engine rebuilt successfully.

endlocal
