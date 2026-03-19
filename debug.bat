@echo off
cd /d "%~dp0"

REM Find MSVC via vswhere and set up cl.exe
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo ERROR: Visual Studio not found
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

REM Build build.exe with TCC, then run debug target with cl available
tcc\tcc.exe -Blib/tcc-windows -o build/Debug/build.exe src/builder/build.c
if errorlevel 1 exit /b 1
build\Debug\build.exe debug
if errorlevel 1 exit /b 1

raddbg\raddbg.exe build/Debug/AnitraEngine.exe "%~dp0games\dungeon1\project.toml"
