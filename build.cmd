@echo off
setlocal
cd /d "%~dp0"
if not exist dist mkdir dist
where cl >nul 2>nul
if errorlevel 1 (
  echo Run from the x64 Native Tools Command Prompt for Visual Studio.
  exit /b 1
)
cl /nologo /std:c++17 /O2 /W4 /WX /EHsc /MT /LD src\health_reader.cpp /Fodist\health_reader.obj /Fedist\SCPInjectHealth.dll /link /MACHINE:X64
exit /b %errorlevel%
