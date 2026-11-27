@echo off
setlocal
rem the script's folder, before shift moves %0 to the arguments
set "ROOT=%~dp0"
set "CONFIG=Release"
set "RUN_TESTS=0"
set "VULKAN=OFF"
set "RECONFIGURE=0"
set "TARGET="
if not defined JOBS set "JOBS=%NUMBER_OF_PROCESSORS%"
if not defined JOBS set "JOBS=4"

:parse
if "%~1"=="" goto build
if /I "%~1"=="Debug" (
    set "CONFIG=Debug"
    shift
    goto parse
)
if /I "%~1"=="Release" (
    set "CONFIG=Release"
    shift
    goto parse
)
if /I "%~1"=="--vulkan" (
    set "VULKAN=ON"
    shift
    goto parse
)
if /I "%~1"=="--test" (
    set "RUN_TESTS=1"
    shift
    goto parse
)
if /I "%~1"=="--reconfigure" (
    set "RECONFIGURE=1"
    shift
    goto parse
)
if /I "%~1"=="--target" (
    if "%~2"=="" goto usage
    set "TARGET=--target %~2"
    shift
    shift
    goto parse
)
if /I "%~1"=="-h" goto help
if /I "%~1"=="--help" goto help
echo Unknown option: %~1 1>&2

:usage
echo Usage: build.cmd [Debug^|Release] [--test] [--vulkan] [--reconfigure] [--target NAME] 1>&2
exit /b 2

:help
echo Usage: build.cmd [Debug^|Release] [--test] [--vulkan] [--reconfigure] [--target NAME]
exit /b 0

:build
pushd "%ROOT%"
if errorlevel 1 exit /b 1
set "CACHE=build\win\CMakeCache.txt"
if not exist "%CACHE%" set "RECONFIGURE=1"
if "%RECONFIGURE%"=="0" (
    findstr /x /c:"XV_DXVK:BOOL=%VULKAN%" "%CACHE%" >nul || set "RECONFIGURE=1"
)
if "%RECONFIGURE%"=="1" (
    cmake -S . -B build\win -G "Visual Studio 17 2022" -A x64 -DXV_DXVK=%VULKAN%
    if errorlevel 1 goto fail
)
if "%RUN_TESTS%"=="1" set "TARGET="
cmake --build build\win --config %CONFIG% --parallel %JOBS% %TARGET% -- /nologo /v:minimal /p:UseMultiToolTask=true /p:EnforceProcessCountAcrossBuilds=true /p:CL_MPCount=%JOBS%
if errorlevel 1 goto fail
if "%RUN_TESTS%"=="1" (
    ctest --test-dir build\win -C %CONFIG% --output-on-failure --parallel %JOBS%
    if errorlevel 1 goto fail
)
echo Built: %CD%\build\win\%CONFIG%\xvalkyrie.exe and xvboot.exe
popd
exit /b 0

:fail
set "EXITCODE=%errorlevel%"
popd
exit /b %EXITCODE%
