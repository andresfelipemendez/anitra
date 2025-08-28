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

:: Use maximum available cores for parallel building
set CORES=%NUMBER_OF_PROCESSORS%
if "%CORES%"=="" set CORES=4
echo Using %CORES% cores for parallel compilation...

:: Build the glad_loader dependency first
echo Building glad_loader dependency...
cmake --build "%BUILD_DIR%" --target glad_loader --config Debug --parallel %CORES%

:: Build the engine target (delete any locked PDB files first)
echo Building the engine target...
echo Cleaning any potentially locked PDB files...
del "%BUILD_DIR%\Debug\engine*.pdb" >NUL 2>&1
cmake --build "%BUILD_DIR%" --target engine --config Debug --parallel %CORES%

:: Check if the build was successful
if %ERRORLEVEL% neq 0 (
    echo Build failed.
    exit /b %ERRORLEVEL%
)

echo Build completed successfully.
echo Engine DLL can be found in %BUILD_DIR%\Debug\

endlocal
