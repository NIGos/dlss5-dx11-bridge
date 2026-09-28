@echo off
setlocal
call "%~dp0vcvars.cmd"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 test-composite.cpp /Fo:build\test-composite.obj /Fe:build\test-composite.exe /link d3d12.lib d3dcompiler.lib dxgi.lib
if errorlevel 1 exit /b 1
build\test-composite.exe
exit /b %errorlevel%
