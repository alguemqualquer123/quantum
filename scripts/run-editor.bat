@echo off
rem ------------------------------------------------------------
rem Run editor ? expects the editor built in %QUANTUM_BUILD_DIR%\<config>\quantum-editor.exe
rem ------------------------------------------------------------
pushd "%~dp0.."

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

call "%~dp0quantum-build-dir.bat"

set EXE=%QUANTUM_BUILD_DIR%\%CONFIG%\quantum-editor.exe
if not exist "%EXE%" (
    echo Editor executable not found at %EXE%. Build the project first.
    popd
    exit /b 1
)
start "" "%EXE%"
popd
pause
