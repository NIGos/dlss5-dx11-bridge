@echo off
setlocal
call "%~dp0vcvars.cmd"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
ml64 /nologo /c /Fo build\present-nr-witness.obj ..\..\src\present-nr-witness.asm
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 test-witness.cpp build\present-nr-witness.obj /Fo:build\test-witness.obj /Fe:build\test-witness.exe
if errorlevel 1 exit /b 1
build\test-witness.exe
exit /b %errorlevel%
