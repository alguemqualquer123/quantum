@echo off
rem ------------------------------------------------------------
rem Clean script ? runs from repository root
rem ------------------------------------------------------------
pushd "%~dp0.."

if exist build rmdir /s /q build
if exist CMakeCache.txt del /f CMakeCache.txt

echo Clean complete.
popd
pause
