@echo off
rem dlss5-bridge -- build. The exact line the README quotes; kept here because it
rem has been retyped by hand once too often and lost a flag each time.
setlocal
rem Honor an explicit toolchain, otherwise use the latest installed C++ tools.
if defined VCVARS goto toolchain_found
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo build.cmd: Visual Studio Installer vswhere.exe was not found; set VCVARS to vcvars64.bat
  exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
:toolchain_found
if not exist "%VCVARS%" (
  echo build.cmd: no vcvars64.bat at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
  echo build.cmd: failed to initialize "%VCVARS%"
  exit /b 1
)
cd /d "%~dp0"

rc /nologo version.rc
if errorlevel 1 exit /b 1

ml64 /nologo /c present-nr-witness.asm
if errorlevel 1 exit /b 1

cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 /Ireshade /Iminhook\include /Iminhook\src /LD ^
   dlss5-bridge.cpp minhook.c version.res present-nr-witness.obj ^
   /Fe:dlss5-bridge.addon64 ^
   /link /DLL user32.lib advapi32.lib bcrypt.lib
if errorlevel 1 exit /b 1
echo built: %~dp0dlss5-bridge.addon64
endlocal
