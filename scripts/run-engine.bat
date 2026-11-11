@echo off
rem ------------------------------------------------------------
rem Run engine - expects the standalone runtime in %QUANTUM_BUILD_DIR%/<config>/quantum-engine.exe
rem ------------------------------------------------------------
pushd "%~dp0.."

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

call "%~dp0quantum-build-dir.bat"

set EXE=%QUANTUM_BUILD_DIR%\%CONFIG%\quantum-engine.exe
if not exist "%EXE%" (
    echo Engine executable not found at %EXE%. Build the project first.
    popd
    exit /b 1
)
start "" "%EXE%"
popd
pause
