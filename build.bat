@echo off
cd /d "%~dp0"
.\tcc -Blib\tcc-windows -o builder.exe build.c 2>&1
if not exist "%~dp0builder.exe" (
    echo ERROR: builder.exe was not created
    exit /b 1
)
"%~dp0builder.exe" %*
