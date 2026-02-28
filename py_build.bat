@echo off
REM Python-based build system wrapper for Anitra Engine
REM Usage: py_build.bat [target]

if "%~1"=="" (
    python build.py all
) else (
    python build.py %1
)
