@echo off
rem ------------------------------------------------------------
rem Run editor ? expects the editor built in %DORIAX_BUILD_DIR%\<config>\doriax-editor.exe
rem ------------------------------------------------------------
pushd "%~dp0.."

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

call "%~dp0doriax-build-dir.bat"

set EXE=%DORIAX_BUILD_DIR%\%CONFIG%\doriax-editor.exe
if not exist "%EXE%" (
    echo Editor executable not found at %EXE%. Build the project first.
    popd
    exit /b 1
)
start "" "%EXE%"
popd
pause
