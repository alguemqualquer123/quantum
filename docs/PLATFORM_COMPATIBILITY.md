# Platform compatibility

| Target | CI gate | Manual gate still required |
| --- | --- | --- |
| Windows MSVC | Release CMake build, install/CPack, engine `--help`, headless Lua/visual-graph smoke | GPU driver matrix, signing and installer upgrade |
| Linux GCC | Release CMake build, install/CPack, engine `--help`, display-free headless Lua/visual-graph smoke | X11/Wayland sessions and vendor GPUs |
| macOS Clang | Release CMake build, package smoke, display-free headless Lua/visual-graph smoke | Metal GPUs, notarization and sandbox |
| Android | Gradle APK assembly with version metadata | Device API/ABI matrix, permissions and store signing |
| iOS/macOS Xcode | Xcode build with version metadata | Provisioning, simulator/device graphics and notarization |
| WebGL2 | Emscripten runtime build and manifest | Browser GPU limits, web audio and hosting headers |

CI proves that the source and packaging contracts are reproducible. It does not
replace a signed-device or browser compatibility pass.
