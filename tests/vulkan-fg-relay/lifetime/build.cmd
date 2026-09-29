@echo off
rem Builds host.exe for the add-on unload/reload regression. Needs VULKAN_SDK.
setlocal
call "%~dp0..\..\vulkan-fg-input\vcvars.cmd"
if errorlevel 1 exit /b 2
if not defined VULKAN_SDK (
  echo build: VULKAN_SDK is not set
  exit /b 2
)
for %%I in ("%~dp0..\..\..\src") do set "SRC=%%~fI"
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /W4 /WX /O2 /EHsc /std:c++17 /MT /I"%VULKAN_SDK%\Include" /I"%SRC%\minhook\include" /I"%SRC%\minhook\src" ^
   host.cpp "%SRC%\minhook.c" /Fo:build\ /Fe:build\host.exe /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib
exit /b %errorlevel%
