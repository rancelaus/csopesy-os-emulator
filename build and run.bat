@echo off
g++ -std=c++11 -pthread main.cpp -o csopesy_cli.exe
if %errorlevel% equ 0 (
    csopesy_cli.exe
) else (
    pause
)