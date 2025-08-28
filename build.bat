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

:: Build the project (clean any PDB files that might be locked)
echo Building the project...
echo Cleaning any potentially locked PDB files...
del "%BUILD_DIR%\Debug\engine*.pdb" >NUL 2>&1
cmake --build "%BUILD_DIR%" --config Debug

:: Check if the build was successful
if %ERRORLEVEL% neq 0 (
    echo Build failed.
    exit /b %ERRORLEVEL%
)

echo Build completed successfully.
echo Executable can be found in %BUILD_DIR%\Debug\

endlocal
