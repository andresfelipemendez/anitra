@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cl /nologo /Fe:build.exe build.c
echo EXITCODE=%ERRORLEVEL%
