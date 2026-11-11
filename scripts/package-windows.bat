@echo off
rem ------------------------------------------------------------
rem Package build ? creates zip in dist directory
rem ------------------------------------------------------------
pushd "%~dp0.."

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

call "%~dp0quantum-build-dir.bat"

set SRC=%QUANTUM_BUILD_DIR%\%CONFIG%
if not exist "%SRC%" (
    echo Build folder %SRC% not found. Run build.bat first.
    popd
    exit /b 1
)

set DIST=dist
if not exist "%DIST%" mkdir "%DIST%"
set ZIP=%DIST%\\Quantum-%CONFIG%.zip
powershell -Command "Compress-Archive -Path '%SRC%\*' -DestinationPath '%ZIP%' -Force"

echo Package created at %ZIP%.
popd
pause
