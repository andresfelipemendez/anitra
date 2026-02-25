@echo off
REM Bootstrap: compile build.c into builder.exe
cd /d "%~dp0"
.\tcc -Blib\tcc-windows -o builder.exe build.c
builder.exe %*
