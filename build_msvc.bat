@echo off
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

where cl >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cl.exe not found. Open an x64 Native Tools Command Prompt for Visual Studio first.
    exit /b 1
)

if not exist "%ROOT%build" mkdir "%ROOT%build"

echo Building command-line tool...
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W4 /permissive- /I "%ROOT%include" ^
   "%ROOT%src\NvChange.cpp" /Fo:"%ROOT%build\\" /Fe:"%ROOT%NvChange-cli.exe"
if errorlevel 1 exit /b 1

echo Building window application...
rc /nologo /fo "%ROOT%build\app.res" "%ROOT%resources.rc"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W4 /permissive- /DUNICODE /D_UNICODE ^
   /I "%ROOT%include" "%ROOT%src\NvChangeGui.cpp" "%ROOT%build\app.res" ^
   /Fo:"%ROOT%build\\" /Fe:"%ROOT%NvChange.exe" ^
   /link /SUBSYSTEM:WINDOWS gdiplus.lib dwmapi.lib ole32.lib uuid.lib
if errorlevel 1 exit /b 1

echo Built: %ROOT%NvChange.exe
echo Built: %ROOT%NvChange-cli.exe
