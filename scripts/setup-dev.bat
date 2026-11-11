@echo off
rem ------------------------------------------------------------
rem Setup development environment ? runs from repository root
rem ------------------------------------------------------------
pushd "%~dp0.."

git submodule update --init --recursive
if exist requirements.txt (
    python -m pip install -r requirements.txt
)

where clang-format >nul 2>&1
if errorlevel 1 (
    echo clang-format not found, attempting pip install...
    python -m pip install clang-format
)

echo Development setup complete.
popd
pause
