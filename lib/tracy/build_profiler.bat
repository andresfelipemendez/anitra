@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cd /d "C:\Users\andres\Development\anitra\lib\tracy"
if exist build-vs rmdir /s /q build-vs
mkdir build-vs
cd build-vs
cmake ..\profiler -G "Ninja" -DCMAKE_BUILD_TYPE=Debug -DDOWNLOAD_CAPSTONE=ON -DDOWNLOAD_GLFW=ON -DDOWNLOAD_FREETYPE=ON 2>&1
if %errorlevel% neq 0 (
    echo === CMAKE CONFIGURE FAILED ===
    exit /b 1
)
echo === Solution ready. Opening in VS ===
cmake --open . 2>&1
