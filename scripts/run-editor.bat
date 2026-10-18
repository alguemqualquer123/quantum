@echo off
rem ------------------------------------------------------------
rem Run editor ? expects executable in build/<config>/editor/editor.exe
rem ------------------------------------------------------------
pushd "%~dp0.."

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set EXE=build\\%CONFIG%\\editor\\editor.exe
if not exist "%EXE%" (
    echo Editor executable not found at %EXE%. Build the project first.
    popd
    exit /b 1
)
start "" "%EXE%"
popd
pause
