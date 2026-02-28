@echo off
REM Simple build wrapper for Anitra Engine
REM Usage: simple_build.bat [target]

setlocal enabledelayedexpansion

REM Parse target argument
set "target=%~1"
if "%target%"=="" set "target=all"

echo === Anitra Engine Build System ===
echo Target: %target%
echo.

REM Ensure build directory exists
if not exist "build" mkdir build
if not exist "build\Debug" mkdir build\Debug

REM Check for TCC compiler
if not exist ".\tcc.exe" (
    echo ERROR: tcc.exe not found!
    echo Please ensure TCC is in the repository root.
    exit /b 1
)

REM Build targets
if "%target%"=="all" call :build_all
if "%target%"=="externals" call :build_externals
if "%target%"=="core" call :build_core
if "%target%"=="engine" call :build_engine
if "%target%"=="editor" call :build_editor
if "%target%"=="exe" call :build_exe
if "%target%"=="clean" call :clean
if "%target%"=="watch" echo Watch mode not implemented in simple build

exit /b 0

REM Build all targets
:build_all
echo Building externals...
call :build_externals || exit /b 1

echo Building core...
call :build_core || exit /b 1

echo Building engine...
call :build_engine || exit /b 1

echo Building editor...
call :build_editor || exit /b 1

echo Building executable...
call :build_exe || exit /b 1

echo Build complete!
exit /b 0

REM Build externals DLL
:build_externals
echo Compiling externals.dll...
.\tcc.exe -Blib\tcc-windows -shared ^
    -o build\Debug\externals.dll ^
    -Isrc -Iinclude ^
    src/externals/externals.c ^
    build\SDL3.def

if not exist "build\Debug\externals.dll" (
    echo ERROR: externals.dll failed to build
    exit /b 1
)
echo externals.dll built successfully
exit /b 0

REM Build core DLL
:build_core
echo Compiling core.dll...
.\tcc.exe -Blib\tcc-windows -shared ^
    -o build\Debug\core.dll ^
    -Isrc ^
    src/core/core.c ^
    src/core/loadlibrary_windows.c ^
    build\SDL3.def ^
    build\externals.def

if not exist "build\Debug\core.dll" (
    echo ERROR: core.dll failed to build
    exit /b 1
)
echo core.dll built successfully
exit /b 0

REM Build engine DLL
:build_engine
echo Compiling engine.dll...
.\tcc.exe -Blib\tcc-windows -shared ^
    -o build\Debug\engine.dll ^
    -Isrc ^
    src/engine/engine.c ^
    src/engine/renderer.c ^
    src/engine/physics.c ^
    src/engine/scene.c ^
    src/engine/debug_render.c ^
    src/engine/anim.c ^
    src/engine/gltf_loader.c ^
    build\SDL3.def

if not exist "build\Debug\engine.dll" (
    echo ERROR: engine.dll failed to build
    exit /b 1
)
echo engine.dll built successfully
exit /b 0

REM Build editor DLL
:build_editor
echo Compiling editor.dll...
.\tcc.exe -Blib\tcc-windows -shared ^
    -o build\Debug\editor.dll ^
    -DCLAY_DISABLE_SIMD ^
    -Isrc ^
    src/editor/editor.c ^
    build\SDL3.def

if not exist "build\Debug\editor.dll" (
    echo ERROR: editor.dll failed to build
    exit /b 1
)
echo editor.dll built successfully
exit /b 0

REM Build executable
:build_exe
echo Compiling AnitraEngine.exe...
.\tcc.exe -Blib\tcc-windows ^
    -o build\Debug\AnitraEngine.exe ^
    -Isrc ^
    src/main.c ^
    src/core/loadlibrary_windows.c ^
    build\SDL3.def

if not exist "build\Debug\AnitraEngine.exe" (
    echo ERROR: AnitraEngine.exe failed to build
    exit /b 1
)
echo AnitraEngine.exe built successfully
exit /b 0

REM Clean build artifacts
:clean
echo Cleaning build artifacts...
if exist "build\Debug\*.dll" del /q build\Debug\*.dll
if exist "build\Debug\*.exe" del /q build\Debug\*.exe
if exist "build\obj" rmdir /s /q build\obj
echo Cleanup complete
exit /b 0
