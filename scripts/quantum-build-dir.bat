@echo off
rem ------------------------------------------------------------
rem QUANTUM_BUILD_DIR: the one build tree this repository uses.
rem
rem Every script, CI job and document reads the folder name from here instead of
rem inventing one, because several defaults (build/, build-debug/, build-release/,
rem build-ninja/) each carried their own CMakeCache, their own generated headers
rem and their own copy of every object file. Set QUANTUM_BUILD_DIR in the
rem environment once - globally, in your shell profile, or per command - to move
rem the whole toolchain to another folder.
rem
rem Called, never run: it sets the variable in the caller's environment.
rem ------------------------------------------------------------
if "%QUANTUM_BUILD_DIR%"=="" set "QUANTUM_BUILD_DIR=build"
exit /b 0
