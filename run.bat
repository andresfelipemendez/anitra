@echo off
cd /d "%~dp0"
tcc\tcc.exe -Blib/tcc-windows -shared -DBUILD_DLL -o build/Debug/build.dll src/builder/build.c
if errorlevel 1 exit /b 1
tcc\tcc.exe -Blib/tcc-windows -o build/Debug/AnitraEngine.exe -Isrc -Isrc/core src/main.c src/core/loadlibrary_windows.c
if errorlevel 1 exit /b 1
start "" "%~dp0lib\tracy\tracy-profiler.exe" -a 127.0.0.1
"%~dp0build\Debug\AnitraEngine.exe" "%~dp0games\dungeon1\project.toml"
