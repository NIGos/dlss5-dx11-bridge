@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 endfield-bounded-report.cpp /Fo:build\bounded.obj /Fe:build\bounded.exe
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 endfield-present-config-gate.cpp /Fo:build\gate.obj /Fe:build\gate.exe
if errorlevel 1 exit /b 1
build\bounded.exe
if errorlevel 1 exit /b 1
build\gate.exe
exit /b %errorlevel%
