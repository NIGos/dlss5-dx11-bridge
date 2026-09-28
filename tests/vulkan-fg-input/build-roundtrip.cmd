@echo off
setlocal
call "%~dp0vcvars.cmd"
if errorlevel 1 exit /b 1
if not defined VULKAN_SDK ( echo VULKAN_SDK is not set & exit /b 1 )
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 /I"%VULKAN_SDK%\Include" roundtrip-host.cpp /Fo:build\roundtrip-host.obj /Fe:build\roundtrip-host.exe /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib user32.lib
exit /b %errorlevel%
