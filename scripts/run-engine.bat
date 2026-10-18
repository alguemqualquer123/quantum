@echo off
rem ------------------------------------------------------------
rem Run engine ? expects executable in build/\u003cconfig\u003e/DoriaX.exe
rem ------------------------------------------------------------
pushd "%~dp0.."

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set EXE=build\\%CONFIG%\\DoriaX.exe
if not exist "%EXE%" (
    echo Engine executable not found at %EXE%. Build the project first.
    popd
    exit /b 1
)
start "" "%EXE%"
popd
pause
