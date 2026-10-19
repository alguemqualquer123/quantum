@echo off
rem ------------------------------------------------------------
rem Run engine - expects the standalone runtime in %DORIAX_BUILD_DIR%/<config>/doriax-engine.exe
rem ------------------------------------------------------------
pushd "%~dp0.."

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

call "%~dp0doriax-build-dir.bat"

set EXE=%DORIAX_BUILD_DIR%\%CONFIG%\doriax-engine.exe
if not exist "%EXE%" (
    echo Engine executable not found at %EXE%. Build the project first.
    popd
    exit /b 1
)
start "" "%EXE%"
popd
pause
