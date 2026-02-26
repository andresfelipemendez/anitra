@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cd /d "C:\Users\andres\Development\anitra\lib\tracy\build-tp"
cmake --build . --config Release -j 8 2>&1
if %errorlevel% neq 0 (
    echo === BUILD FAILED ===
    exit /b 1
)
echo === BUILD SUCCEEDED ===
copy /Y tracy-profiler.exe ..\tracy-profiler.exe
