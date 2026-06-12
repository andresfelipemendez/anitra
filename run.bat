@echo off
cd /d "%~dp0"
call "%~dp0build.bat"
if errorlevel 1 exit /b 1
"%~dp0build\Debug\AnitraEngine.exe" "%~dp0games\dungeon1\project.toml"
