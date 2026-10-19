@echo off
rem ------------------------------------------------------------
rem Format C/C++ source files using clang-format ? runs from repository root
rem ------------------------------------------------------------
pushd "%~dp0.."

where clang-format >nul 2>&1
if errorlevel 1 (
    echo clang-format not found in PATH. Install it first.
    popd
    exit /b 1
)

for /r . %%F in (*.c *.cpp *.h *.hpp) do (
    echo Formatting %%F
    clang-format -i "%%F"
)

echo Formatting completed.
popd
pause
