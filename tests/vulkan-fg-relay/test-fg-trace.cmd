@echo off
rem CPU-only contract test for the Trace=1 observers (fg-trace.inc) compiled together
rem with the whole add-on source. No game, GPU, Vulkan device or NGX module is used.
rem Usage: test-fg-trace.cmd <new-output-directory>   (an existing directory is refused)
setlocal
if "%~1"=="" (
  echo usage: test-fg-trace.cmd ^<new-output-directory^>
  exit /b 2
)
call "%~dp0..\vulkan-fg-input\vcvars.cmd"
if errorlevel 1 exit /b 2
for %%I in ("%~dp0..\..\src") do set "SRC=%%~fI"
set "OUT=%~f1"
mkdir "%OUT%" 2>nul
if errorlevel 1 (
  echo test-fg-trace: cannot create "%OUT%" ^(it must not exist yet^)
  exit /b 2
)
mkdir "%OUT%\build"
pushd "%OUT%\build"
ml64 /nologo /c /Fo present-nr-witness.obj "%SRC%\present-nr-witness.asm" > "%OUT%\build.log" 2>&1
if errorlevel 1 goto failed
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 /I"%SRC%\reshade" /I"%SRC%\minhook\include" /I"%SRC%\minhook\src" ^
  "%~dp0test-fg-trace.cpp" "%SRC%\minhook.c" present-nr-witness.obj ^
  /Fe:test-fg-trace.exe /link user32.lib advapi32.lib bcrypt.lib >> "%OUT%\build.log" 2>&1
if errorlevel 1 goto failed
popd
rem The add-on reads vk-present-adapter.ini beside its own module, so the test
rem executable and the INI sit in the same directory.
copy /b "%OUT%\build\test-fg-trace.exe" "%OUT%\test-fg-trace.exe" >nul
if errorlevel 1 exit /b 2
(
  echo [Adapter]
  echo Enabled=1
  echo Source=fg-input
  echo Trace=1
  echo Pipeline=0
) > "%OUT%\vk-present-adapter.ini"
pushd "%OUT%"
"%OUT%\test-fg-trace.exe" > run.txt 2>&1
set "RESULT=%errorlevel%"
type run.txt
popd
exit /b %RESULT%
:failed
popd
type "%OUT%\build.log"
exit /b 2
