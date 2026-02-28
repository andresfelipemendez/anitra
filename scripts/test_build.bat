@echo off
cd /d "%~dp0"
..\build.bat clean
..\build.bat externals
..\build.bat core
..\build.bat engine
..\build.bat editor
..\build.bat exe

if exist "..\build\Debug\AnitraEngine.exe" (
    echo Build successful!
    echo Launching game...
    start "" "..\build\Debug\AnitraEngine.exe"
) else (
    echo Build failed!
)
