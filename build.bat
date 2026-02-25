@echo off
REM Bootstrap: compile build.c into build.exe, then run it.
REM After first run, you can use build.exe directly.
for /f "delims=" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath') do set VSDIR=%%i
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cd /d "%~dp0"
cl /nologo "%~dp0build.c" /Fe:"%~dp0build.exe"
"%~dp0build.exe" %*
