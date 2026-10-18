@echo off
rem ------------------------------------------------------------
rem Build script for DoriaX ? runs from the repository root
rem ------------------------------------------------------------
pushd "%~dp0.."

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

if not exist build mkdir build

cmake -S . -B build -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 (echo CMake configuration failed & popd & exit /b 1)

cmake --build build --config %CONFIG%
if errorlevel 1 (echo Build failed & popd & exit /b 1)

echo Build completed.
popd
pause
