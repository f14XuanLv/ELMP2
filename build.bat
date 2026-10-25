@echo off
chcp 65001 >nul
setlocal EnableExtensions EnableDelayedExpansion

rem Configure and build ELMP against the pinned RE-UE4SS submodule (does not modify RE-UE4SS).
rem
rem   build.bat            configure (if needed) and build ELMP_Dev, ELMP, ElmpFrameProbe and the CTests
rem   build.bat test       build, then run CTest
rem   build.bat clean      remove build\, then configure and build
rem   build.bat package    build, then tools\package.py (Shipping zip asserted free of .lua)
rem   build.bat base       only elmp_base + CTests (no UE4SS build)
rem
rem Output: build\<cfg>\{dev,shipping,frameprobe}\dlls\main.dll. Deploy with tools\deploy.py.
rem Paths come from devconfig.json (+ devconfig.local.json).

set "ROOT_BS=%~dp0"
set "ROOT=%ROOT_BS:~0,-1%"
set "BUILD_DIR=%ROOT%\build"
if not defined ELMP_BUILD_CONFIG set "ELMP_BUILD_CONFIG=Game__Shipping__Win64"
set "MSVC_TOOLSET_VERSION=14.44"

for /f "usebackq delims=" %%I in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$c = Get-Content -Raw -Encoding UTF8 '%ROOT%\devconfig.json' | ConvertFrom-Json; $l = Join-Path '%ROOT%' 'devconfig.local.json'; if (Test-Path $l) { $o = Get-Content -Raw -Encoding UTF8 $l | ConvertFrom-Json; foreach ($p in $o.PSObject.Properties) { $c | Add-Member -Force -NotePropertyName $p.Name -NotePropertyValue $p.Value } }; Write-Output $c.python; Write-Output ('fetch=' + $c.fetchCache)"`) do (
    set "LINE=%%I"
    if "!LINE:~0,6!"=="fetch=" (
        if not defined ELMP_FETCH_CACHE if not "!LINE:~6!"=="" set "ELMP_FETCH_CACHE=!LINE:~6!"
    ) else (
        set "PYTHON=%%I"
    )
)
if defined ELMP_FETCH_CACHE set "CARGO_NET_OFFLINE=true"

set "DO_CLEAN=0"
set "DO_TEST=0"
set "DO_PACKAGE=0"
set "TARGETS=ELMP_Dev ELMP ElmpFrameProbe test_manifest test_storage test_bus_slots"
for %%A in (%*) do (
    if /I "%%~A"=="clean" set "DO_CLEAN=1"
    if /I "%%~A"=="test" set "DO_TEST=1"
    if /I "%%~A"=="package" set "DO_PACKAGE=1"
    if /I "%%~A"=="base" set "TARGETS=elmp_base test_manifest test_storage test_bus_slots"
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" echo [ERROR] vswhere.exe not found && exit /b 1
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -property installationPath`) do set "VS=%%I"
if not defined VS echo [ERROR] Visual Studio not found via vswhere && exit /b 1
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=%MSVC_TOOLSET_VERSION% >nul
if errorlevel 1 echo [ERROR] vcvars64 failed && exit /b 1

if not exist "%ROOT%\RE-UE4SS\CMakeLists.txt" echo [ERROR] RE-UE4SS submodule missing (git submodule update --init --recursive) && exit /b 1

if "%DO_CLEAN%"=="1" if exist "%BUILD_DIR%" (
    echo [clean] removing %BUILD_DIR%
    rmdir /S /Q "%BUILD_DIR%"
)

if not exist "%BUILD_DIR%\build.ninja" (
    echo [configure] cmake -G Ninja -DCMAKE_BUILD_TYPE=%ELMP_BUILD_CONFIG%
    cmake -G Ninja -DCMAKE_BUILD_TYPE=%ELMP_BUILD_CONFIG% -B "%BUILD_DIR%" -S "%ROOT%" >"%ROOT%\build.configure.log" 2>&1
    if errorlevel 1 type "%ROOT%\build.configure.log" && exit /b 1
)

echo [build] %TARGETS%
cmake --build "%BUILD_DIR%" --target %TARGETS% -j 8
if errorlevel 1 exit /b 1

if "%DO_TEST%"=="1" (
    ctest --test-dir "%BUILD_DIR%" --output-on-failure
    if errorlevel 1 exit /b 1
)

if "%DO_PACKAGE%"=="1" (
    "%PYTHON%" "%ROOT%\tools\package.py" --config %ELMP_BUILD_CONFIG%
    if errorlevel 1 exit /b 1
)

echo [done] %BUILD_DIR%\%ELMP_BUILD_CONFIG%\{dev,shipping,frameprobe}\dlls\main.dll
endlocal
exit /b 0
