@echo off
rem Builds scan-test.exe and the fixture DLLs it loads, all into a new output directory.
rem scan-test.exe tests src/ngx-module-scan.h against the real Windows loader.
rem Usage: build-scan-test.cmd <new-output-directory>, then run <dir>\scan-test.exe
setlocal EnableExtensions DisableDelayedExpansion
if "%~1"=="" (
  echo usage: build-scan-test.cmd ^<new-output-directory^>
  exit /b 2
)
call "%~dp0..\..\vulkan-fg-input\vcvars.cmd"
if errorlevel 1 exit /b 2
set "OUT=%~f1"
mkdir "%OUT%" 2>nul
if errorlevel 1 (
  echo build-scan-test: cannot create "%OUT%" ^(it must not exist yet^)
  exit /b 2
)
pushd "%OUT%"
set "FLAGS=/nologo /W4 /WX /O2 /EHsc /std:c++17 /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX"
cl %FLAGS% /c "%~dp0scan-fixture.cpp" /Fo:scan-fixture.obj
if errorlevel 1 goto failed
cl %FLAGS% /DSCAN_FORWARD_TARGET /c "%~dp0scan-fixture.cpp" /Fo:scan-target.obj
if errorlevel 1 goto failed

set "D11C=/EXPORT:NVSDK_NGX_D3D11_CreateFeature=ScanFixtureValue"
set "D11E=/EXPORT:NVSDK_NGX_D3D11_EvaluateFeature=ScanFixtureValue"
set "D11EC=/EXPORT:NVSDK_NGX_D3D11_EvaluateFeature_C=ScanFixtureValue"
set "VKC=/EXPORT:NVSDK_NGX_VULKAN_CreateFeature=ScanFixtureValue"
set "VKC1=/EXPORT:NVSDK_NGX_VULKAN_CreateFeature1=ScanFixtureValue"
set "VKE=/EXPORT:NVSDK_NGX_VULKAN_EvaluateFeature=ScanFixtureValue"
set "VKEC=/EXPORT:NVSDK_NGX_VULKAN_EvaluateFeature_C=ScanFixtureValue"

call :fixture plain
if errorlevel 1 goto failed
call :fixture d11-create "%D11C%"
if errorlevel 1 goto failed
call :fixture d11-eval "%D11E%"
if errorlevel 1 goto failed
call :fixture d11-eval-c "%D11EC%"
if errorlevel 1 goto failed
call :fixture vk-create "%VKC%"
if errorlevel 1 goto failed
call :fixture vk-create1 "%VKC1%"
if errorlevel 1 goto failed
call :fixture vk-eval "%VKE%"
if errorlevel 1 goto failed
call :fixture vk-eval-c "%VKEC%"
if errorlevel 1 goto failed
call :fixture cross-d11 "%D11C%" "%VKE%"
if errorlevel 1 goto failed
call :fixture cross-vk "%VKC%" "%D11E%"
if errorlevel 1 goto failed
call :fixture near "%D11C%" "/EXPORT:NVSDK_NGX_D3D11_EvaluateFeature_C_extra=ScanFixtureValue" "/EXPORT:NVSDK_NGX_D3D11_evaluatefeature=ScanFixtureValue" "/EXPORT:NVSDK_NGX_VULKAN_CreateFeature10=ScanFixtureValue" "%VKE%"
if errorlevel 1 goto failed
call :fixture d11 "%D11C%" "%D11E%"
if errorlevel 1 goto failed
call :fixture d11-c "%D11C%" "%D11EC%"
if errorlevel 1 goto failed
call :fixture vk "%VKC%" "%VKE%"
if errorlevel 1 goto failed
call :fixture vk-c "%VKC%" "%VKEC%"
if errorlevel 1 goto failed
call :fixture vk1 "%VKC1%" "%VKE%"
if errorlevel 1 goto failed
call :fixture vk1-c "%VKC1%" "%VKEC%"
if errorlevel 1 goto failed
link /nologo /WX /DLL /MACHINE:X64 /INCREMENTAL:NO /OUT:scan-target.dll /IMPLIB:scan-target.lib scan-target.obj kernel32.lib
if errorlevel 1 goto failed
call :fixture forward "/EXPORT:NVSDK_NGX_D3D11_CreateFeature=scan-target.ScanFixtureValue" "/EXPORT:NVSDK_NGX_D3D11_EvaluateFeature=scan-target.ScanFixtureValue" "/EXPORT:NVSDK_NGX_D3D11_EvaluateFeature_C=scan-target.ScanFixtureValue" "/EXPORT:NVSDK_NGX_VULKAN_CreateFeature=scan-target.ScanFixtureValue" "/EXPORT:NVSDK_NGX_VULKAN_CreateFeature1=scan-target.ScanFixtureValue" "/EXPORT:NVSDK_NGX_VULKAN_EvaluateFeature=scan-target.ScanFixtureValue" "/EXPORT:NVSDK_NGX_VULKAN_EvaluateFeature_C=scan-target.ScanFixtureValue"
if errorlevel 1 goto failed
cl %FLAGS% "%~dp0scan-test.cpp" /Fo:scan-test.obj /Fe:scan-test.exe /link /WX /INCREMENTAL:NO
if errorlevel 1 goto failed
echo built: %OUT%\scan-test.exe
popd
exit /b 0

:fixture
link /nologo /WX /DLL /MACHINE:X64 /INCREMENTAL:NO /OUT:scan-%~1.dll /IMPLIB:scan-%~1.lib scan-fixture.obj %2 %3 %4 %5 %6 %7 %8 kernel32.lib
exit /b %errorlevel%

:failed
popd
exit /b 1
