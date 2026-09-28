@echo off
rem Shared toolchain lookup for the tests: honor VCVARS, else the latest installed VC++ tools.
if defined VCVARS goto found
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo vswhere.exe not found; set VCVARS to vcvars64.bat & exit /b 1 )
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
:found
if not exist "%VCVARS%" ( echo no vcvars64.bat at "%VCVARS%" & exit /b 1 )
call "%VCVARS%" >nul 2>&1
