@echo off
setlocal
cd /d "%~dp0"

rem Use the installed thread-capable Dev-C++ compiler when available.
rem Set CXX to a compiler executable path to override this selection.
set "CSOPESY_CXX=g++"
if exist "%ProgramFiles(x86)%\Dev-Cpp\MinGW64\bin\g++.exe" set "CSOPESY_CXX=%ProgramFiles(x86)%\Dev-Cpp\MinGW64\bin\g++.exe"
if defined CXX set "CSOPESY_CXX=%CXX%"

echo Building with "%CSOPESY_CXX%"
"%CSOPESY_CXX%" -std=c++11 -pthread -static main.cpp -o csopesy_cli.exe
if errorlevel 1 (
    echo Build failed. Use a compiler with C++11 thread, mutex, and condition_variable support.
    echo The older MinGW.org compiler may not provide these types, even with -pthread.
    pause
    exit /b 1
)

csopesy_cli.exe
exit /b %errorlevel%
