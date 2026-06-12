@echo off
cd /d "%~dp0"
if not exist build\Debug mkdir build\Debug
tcc\tcc.exe -Blib/tcc-windows -shared -DBUILD_DLL -o build/Debug/build.dll src/builder/build.c
if errorlevel 1 exit /b 1
tcc\tcc.exe -Blib/tcc-windows -o build/Debug/AnitraEngine.exe -Isrc -Isrc/core src/main.c src/core/loadlibrary_windows.c
if errorlevel 1 exit /b 1
echo Build OK.
