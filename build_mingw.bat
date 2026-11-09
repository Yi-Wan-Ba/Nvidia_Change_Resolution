@echo off
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"
if not defined MINGW_BIN set "MINGW_BIN=%ROOT%.tools\mingw\mingw64\bin"
if exist "%MINGW_BIN%\g++.exe" set "PATH=%MINGW_BIN%;%PATH%"

where g++ >nul 2>nul
if errorlevel 1 (
    echo [ERROR] g++ not found. Install MinGW-w64 or set MINGW_BIN to its bin directory.
    exit /b 1
)

if not exist "%ROOT%build" mkdir "%ROOT%build"

echo Building command-line tool...
g++ -std=c++20 -O2 -Wall -Wextra -static -I "%ROOT%include" ^
    "%ROOT%src\NvChange.cpp" -o "%ROOT%NvChange-cli.exe"
if errorlevel 1 exit /b 1

echo Building window application...
windres -I "%ROOT%include" -i "%ROOT%resources.rc" -o "%ROOT%build\app_res.o"
if errorlevel 1 exit /b 1
g++ -std=c++20 -O2 -Wall -Wextra -static -mwindows -I "%ROOT%include" ^
    "%ROOT%src\NvChangeGui.cpp" "%ROOT%build\app_res.o" ^
    -o "%ROOT%NvChange.exe" -lgdiplus -ldwmapi -lole32 -luuid
if errorlevel 1 exit /b 1

echo Built: %ROOT%NvChange.exe
echo Built: %ROOT%NvChange-cli.exe
