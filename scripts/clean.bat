@echo off
rem ------------------------------------------------------------
rem Clean script ? runs from repository root
rem ------------------------------------------------------------
pushd "%~dp0.."

rem Only a tree CMake configured is removed, so a wrong DORIAX_BUILD_DIR cannot
rem point rmdir at an unrelated folder.
call "%~dp0doriax-build-dir.bat"
if exist "%DORIAX_BUILD_DIR%\CMakeCache.txt" rmdir /s /q "%DORIAX_BUILD_DIR%"
if exist CMakeCache.txt del /f CMakeCache.txt

echo Clean complete.
popd
pause
