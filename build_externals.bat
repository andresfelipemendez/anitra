@echo off
setlocal enabledelayedexpansion

:: Set the project root directory (assuming the script is in the project root)
set "PROJECT_ROOT=%~dp0"
set "BUILD_DIR=%PROJECT_ROOT%build"

:: Check if build directory exists
if not exist "%BUILD_DIR%" (
    echo Build directory does not exist. Please run generate.bat first.
    exit /b 1
)

:: Build the glad_loader dependency first
echo Building glad_loader dependency...
cmake --build "%BUILD_DIR%" --target glad_loader --config Debug

:: Build the externals target
echo Building the externals target...
cmake --build "%BUILD_DIR%" --target externals --config Debug

:: Check if the build was successful
if %ERRORLEVEL% neq 0 (
    echo Build failed.
    exit /b %ERRORLEVEL%
)

echo Build completed successfully.
echo Externals DLL can be found in %BUILD_DIR%\Debug\

endlocal