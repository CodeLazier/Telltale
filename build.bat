@echo off
setlocal
cd /d "%~dp0"

where g++ >nul 2>nul
if errorlevel 1 (
    echo g++ not found in PATH.
    echo Install MSYS2 / MinGW-w64 and add the ucrt64\bin directory to PATH.
    exit /b 1
)

where windres >nul 2>nul
if errorlevel 1 (
    echo windres not found in PATH.
    echo It ships with MSYS2 / MinGW-w64 next to g++. Add the ucrt64\bin directory to PATH.
    exit /b 1
)

rem Exit the tray app before building; the exe is locked while it is running.
set "TARGET=%LOCALAPPDATA%\Programs\Telltale"
if not exist "%TARGET%" mkdir "%TARGET%"

rem Embed src\sun.ico into the exe (tray icon + file icon).
set "RES=%TEMP%\telltale_res_%RANDOM%.o"
windres -I src -O coff src\app.rc -o "%RES%"
if errorlevel 1 exit /b 1

g++ -std=c++17 -O2 -municode -mwindows -static -o "%TARGET%\telltale.exe" src\telltale.cpp "%RES%" ^
    -limm32 -luser32 -lgdi32 -lshell32 -lgdiplus -ladvapi32 -lole32 -loleaut32 ^
    -loleacc -luuid -luiautomationcore
set "RC=%ERRORLEVEL%"
del "%RES%" >nul 2>nul
if not "%RC%"=="0" exit /b %RC%

echo Installed: %TARGET%\telltale.exe
