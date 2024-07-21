@echo off
setlocal enabledelayedexpansion

:: Set the project root directory (assuming the script is in the project root)
set "PROJECT_ROOT=%~dp0"
set "BUILD_DIR=%PROJECT_ROOT%build"

:: Create the build directory if it doesn't exist
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

:: Run CMake to generate the project files for x64
echo Generating project files...
cmake -G "Visual Studio 17 2022" -A x64 -B "%BUILD_DIR%" -S "%PROJECT_ROOT%"

:: Check if CMake was successful
if %ERRORLEVEL% neq 0 (
    echo CMake configuration failed.
    exit /b %ERRORLEVEL%
)

echo CMake configuration completed successfully.
echo Build files have been generated in %BUILD_DIR%

endlocal
