@echo off
REM Build helpers for Anitra Engine

REM Helper to check if file needs rebuilding
:needs_rebuild
set "src=%~1"
set "obj=%~2"

if not exist "%obj%" (
    exit /b 0  % Need rebuild - obj doesn't exist
)

for %%A in ("%src%") do for %%B in ("%obj%") do (
    if %%~tA GTR %%~tB (
        exit /b 0  % Need rebuild - src is newer
    )
)
exit /b 1  % No rebuild needed

REM Helper to ensure directory exists
:ensure_dir
if not exist "%~1" (
    mkdir "%~1"
    if errorlevel 1 (
        echo ERROR: Failed to create directory: %~1
        exit /b 1
    )
)
exit /b 0

REM Helper to run a command and check result
:run_cmd
echo >> %~1
%~2
if errorlevel 1 (
    echo ERROR: Command failed: %~2
    exit /b 1
)
exit /b 0
