@echo off
setlocal EnableExtensions
rem Package a Release Windows build with Inno Setup.
rem From a Visual Studio build:
rem   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
rem   cmake --build build --config Release --target GuitarDelay_VST3 GuitarDelay_Standalone
rem   installer\windows\build-windows.cmd
rem Optional argument: path to the CMake build directory (default: build).

set "ROOT=%~dp0..\.."
pushd "%ROOT%"
set "BUILD=%CD%\build"
if not "%~1"=="" set "BUILD=%~f1"

set "DIST=%BUILD%\dist"
set "VST3_DIR=%BUILD%\GuitarDelay_artefacts\Release\VST3"
set "VST3=%VST3_DIR%\Tidelight Delay.vst3"
set "APP=%BUILD%\GuitarDelay_artefacts\Release\Standalone\Tidelight Delay.exe"

set "VERSION="
for /f "tokens=3" %%V in ('findstr /B /C:"project(GuitarDelay VERSION " CMakeLists.txt') do set "VERSION=%%V"
if not defined VERSION (
    echo Could not read the release version from CMakeLists.txt.
    popd
    exit /b 1
)

if not exist "%VST3%\Contents\x86_64-win\Tidelight Delay.vst3" (
    echo Could not find "%VST3%".
    echo Build the Release VST3 target first. The repository does not contain a prebuilt plugin.
    popd
    exit /b 1
)

if not exist "%APP%" (
    echo Could not find "%APP%". Build the Release Standalone target first.
    popd
    exit /b 1
)

if not defined ISCC set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
    echo Inno Setup 6 is not installed.
    echo Download it from https://jrsoftware.org/isdl.php and rerun this script.
    popd
    exit /b 1
)

"%ISCC%" "/DVst3Bundle=%VST3%" "/DStandaloneExe=%APP%" "/DOutputDir=%DIST%" "/DMyAppVersion=%VERSION%" "%CD%\installer\windows\GuitarDelay.iss"
set "ERR=%ERRORLEVEL%"
if not "%ERR%"=="0" (
    popd
    exit /b %ERR%
)

set "VST3_ZIP=%DIST%\TidelightDelay-VST3-%VERSION%.zip"
tar.exe -a -cf "%VST3_ZIP%" -C "%VST3_DIR%" "Tidelight Delay.vst3"
set "ERR=%ERRORLEVEL%"
if not "%ERR%"=="0" (
    echo Could not write "%VST3_ZIP%".
    popd
    exit /b %ERR%
)
echo VST3 zip: %VST3_ZIP%
set "SUM=%DIST%\TidelightDelay-%VERSION%.sha256"
set "SETUP_EXE=%DIST%\TidelightDelay-Setup-%VERSION%.exe"
set "PSModulePath="
powershell.exe -NoProfile -Command "$ErrorActionPreference = 'Stop'; Get-FileHash -Algorithm SHA256 -LiteralPath $env:SETUP_EXE,$env:VST3_ZIP | ForEach-Object { '{0}  {1}' -f $_.Hash.ToLowerInvariant(), (Split-Path $_.Path -Leaf) } | Set-Content -Encoding ascii -LiteralPath $env:SUM"
set "ERR=%ERRORLEVEL%"
if not "%ERR%"=="0" (
    echo Could not write "%SUM%".
    popd
    exit /b %ERR%
)
echo Checksums: %SUM%
popd
exit /b 0
