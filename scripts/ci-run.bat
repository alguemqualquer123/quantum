@echo off
rem ------------------------------------------------------------
rem CI run ? builds Release then runs tests
rem ------------------------------------------------------------
pushd "%~dp0.."

call build.bat Release
if errorlevel 1 (popd & exit /b 1)

call test.bat
popd
pause
