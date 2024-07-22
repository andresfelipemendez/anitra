@echo off
cmake -S . -B ninja -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

