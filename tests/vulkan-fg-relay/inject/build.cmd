@echo off
rem Builds host.exe for the device-extension injection / shared-fence probe. Needs VULKAN_SDK.
setlocal
call "%~dp0..\..\vulkan-fg-input\vcvars.cmd"
if errorlevel 1 exit /b 2
if not defined VULKAN_SDK (
  echo build: VULKAN_SDK is not set
  exit /b 2
)
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 /I"%VULKAN_SDK%\Include" host.cpp ^
   /Fo:build\ /Fe:build\host.exe /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib d3d12.lib user32.lib
exit /b %errorlevel%
