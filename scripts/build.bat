@echo off
rem ------------------------------------------------------------
rem Build script for DoriaX ? runs from the repository root
rem ------------------------------------------------------------
pushd "%~dp0.."

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

call "%~dp0doriax-build-dir.bat"

if not exist "%DORIAX_BUILD_DIR%" mkdir "%DORIAX_BUILD_DIR%"

cmake -S . -B "%DORIAX_BUILD_DIR%" -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 (echo CMake configuration failed & popd & exit /b 1)

cmake --build "%DORIAX_BUILD_DIR%" --config %CONFIG%
if errorlevel 1 (echo Build failed & popd & exit /b 1)

echo Build completed.
popd
pause
