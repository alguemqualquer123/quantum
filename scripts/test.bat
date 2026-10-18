@echo off
rem ------------------------------------------------------------
rem Test script ? runs from repository root
rem ------------------------------------------------------------
pushd "%~dp0.."

if not exist build (
    echo Build directory not found. Run build.bat first.
    popd
    exit /b 1
)

cd build
ctest --output-on-failure
cd ..
popd
pause
