@echo off
rem ------------------------------------------------------------
rem Recompile shaders ? runs from repository root
rem ------------------------------------------------------------
pushd "%~dp0.."

set COMPILER=shadercompiler\\shadercompiler.exe
if not exist "%COMPILER%" (
    echo Shader compiler not found at %COMPILER%.
    popd
    exit /b 1
)

for /r shaders %%F in (*.vert *.frag *.comp *.glsl) do (
    echo Compiling %%F...
    "%COMPILER%" "%%F"
)

echo Shader compilation completed.
popd
pause
