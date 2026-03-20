@echo off
cd /d "%~dp0"

REM Find MSVC via vswhere and set up cl.exe
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo ERROR: Visual Studio not found
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

REM Build build.exe and build.dll with TCC, then run target with cl/clang-cl available
tcc\tcc.exe -Blib/tcc-windows -o build/Debug/build.exe src/builder/build.c
if errorlevel 1 exit /b 1
tcc\tcc.exe -Blib/tcc-windows -shared -DBUILD_DLL -o build/Debug/build.dll src/builder/build.c
if errorlevel 1 exit /b 1

REM Default target: debug. Pass "coverage" for coverage report.
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=debug"
build\Debug\build.exe %TARGET%
if errorlevel 1 exit /b 1

REM Launch debugger with ANITRA_SKIP_BUILD so the exe doesn't rebuild with TCC
if "%TARGET%"=="debug" (
    set "ANITRA_SKIP_BUILD=1"
    C:\Users\andres\bin\remedybg_0_4_0_13\remedybg.exe build/Debug/AnitraEngine.exe "%~dp0games\dungeon1\project.toml"
)
