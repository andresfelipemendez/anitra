@echo off
cd /d "%~dp0"
taskkill /F /IM AnitraEngine.exe >nul 2>&1
taskkill /F /IM collab_server.exe >nul 2>&1
taskkill /F /IM builder.exe >nul 2>&1
.\tcc -Blib\tcc-windows -o builder.exe build.c 2>&1
if not exist "%~dp0builder.exe" (
    echo ERROR: builder.exe was not created
    exit /b 1
)
"%~dp0builder.exe" %*
