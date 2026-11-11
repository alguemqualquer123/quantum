@echo off
rem ------------------------------------------------------------
rem Test script ? runs from repository root
rem ------------------------------------------------------------
pushd "%~dp0.."

call "%~dp0quantum-build-dir.bat"

if not exist "%QUANTUM_BUILD_DIR%" (
    echo Build directory not found. Run build.bat first.
    popd
    exit /b 1
)

cd "%QUANTUM_BUILD_DIR%"
ctest --output-on-failure
cd ..
popd
pause
