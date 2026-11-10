@echo off
rem Builds the Windows plugin and runs the self-test (see WINDOWS.md).
rem Needs Visual Studio 2022 with "Desktop development with C++", CMake, and the CUDA Toolkit.
rem
rem   build-windows.bat            build into build-win\ and run the self-test
rem   build-windows.bat install    then install into C:\Program Files\Common Files\OFX\Plugins
rem                                (run from an administrator Command Prompt, with Resolve closed)
rem Set LC_NOPAUSE=1 to skip the "press a key" pause on failure (for scripted runs).
setlocal
rem pushd also works when this folder is on a network share (it maps a temporary drive letter)
pushd "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake was not found. Install it with:  winget install Kitware.CMake
    echo or run this from the "Developer Command Prompt for VS 2022".
    goto fail
)

cmake -S . -B build-win -A x64
if errorlevel 1 goto fail
cmake --build build-win --config Release
if errorlevel 1 goto fail

set "LENSCHARACTER_PROFILES=%CD%\build-win\LensCharacter.ofx.bundle\Contents\Resources\profiles"
build-win\bin\selftest.exe build-win
if errorlevel 1 goto fail

if /i "%~1"=="install" (
    cmake --install build-win --config Release
    if errorlevel 1 goto fail
)

echo.
echo Done. The plugin is in %CD%\build-win\LensCharacter.ofx.bundle
popd
exit /b 0

:fail
echo.
echo Build failed. Copy the messages above into the chat.
popd
if not defined LC_NOPAUSE pause
exit /b 1
