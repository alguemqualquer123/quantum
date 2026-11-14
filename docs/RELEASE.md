# Release and packaging guide

The release pipeline is split between a desktop CMake package and native
mobile/web packages. All release gates are headless.

## Desktop

```powershell
$env:QUANTUM_BUILD_DIR = "build"
cmake -S . -B $env:QUANTUM_BUILD_DIR -DQUANTUMEDITOR_VERSION=v0.1.0
cmake --build $env:QUANTUM_BUILD_DIR --config Release --target quantum-editor quantum-editor-cmd quantum-engine quantum-network-relay quantum-resource-compiler quantum-build-generator
cmake --install $env:QUANTUM_BUILD_DIR --config Release --prefix stage --strip
cpack --config "$env:QUANTUM_BUILD_DIR/CPackConfig.cmake" --output-directory dist
python scripts/release-smoke.py --root stage --manifest "stage/release-manifest.json" \
    --executable stage/bin/quantum-engine.exe \
    --editor-cmd stage/bin/quantum-editor-cmd.exe \
    --relay-executable stage/bin/quantum-network-relay.exe \
    --resource-server stage/bin/quantum-resource-server.exe \
    --resource-compiler stage/bin/quantum-resource-compiler.exe \
    --build-generator stage/bin/quantum-build-generator.exe \
    --runtime-project templates/daynight \
    --resource-project tests/scale/probe \
    --report stage/release-smoke-report.json
```

Windows produces ZIP, Linux TGZ and macOS DragNDrop packages. The generated
manifest is installed beside the binaries and records platform, architecture,
channel, backend and version.
The manifest is written during `cmake --install`, so multi-config generators
such as Visual Studio record the configuration actually installed (`Release`,
`RelWithDebInfo` or `MinSizeRel`) instead of the configuration used at the
initial configure step.
Windows/MSVC shared builds also install the toolchain-matched VC143 runtime DLLs
beside `quantum.dll`, so an extracted ZIP does not depend on the developer's
Visual Studio environment.
The desktop package also installs `quantum-network-relay.exe` for authenticated
UDP room registration and forwarding; relay secrets, hosting and regional
topology remain deployment configuration.
With `--runtime-project`, the gate executes the installed engine (not the build-tree
copy) with the Lua and visual-graph smokes. With `--resource-project`, it starts the
installed resource server with `--once` and checks its persisted project index.
The headless `quantum-resource-compiler` and `quantum-build-generator` are also
built and installed on Windows, Linux and macOS; only their Windows icon and
manifest are platform-specific.
The headless smoke also writes `release-smoke-report.json` with SHA-256 and size
for every verified artifact. It accepts both files and bundles/directories, so
the same gate covers desktop binaries, Android APKs, WebGL outputs and Apple
`.app` and `.ipa` products before publishing. Apple bundles must have a valid
`Info.plist`, a non-empty `CFBundleIdentifier`, and a non-empty executable named
by `CFBundleExecutable`; `.ipa` archives must contain exactly one such app under
`Payload/`. Before inspecting artifacts, the validator also requires a schema-1
Quantum manifest with non-empty channel, platform, architecture, graphic backend
and build type, and rejects impossible platform/backend pairs such as Android
with Metal or Web with Vulkan.

## Mobile and web

Android uses `./gradlew --no-daemon assembleRelease -PQUANTUM_VERSION=v0.1.0
-PQUANTUM_VERSION_CODE=1`. Xcode receives `MARKETING_VERSION` and
`CURRENT_PROJECT_VERSION`; Emscripten publishes the runtime and its manifest.
These steps do not sign or upload to a store. CI runs the same report gate with
`--artifact` for the APK, Web output and Apple app bundle; the report is stored
next to each uploaded artifact.

## Gates

Run `python scripts/check-portability.py` before configuring. CI then builds the
desktop matrix, Android and WebGL2 smoke targets, runs `--help` and executes
`quantum-engine --project templates/daynight --headless --script-smoke --graph-smoke
--graph-project-smoke --frames 1 --warmup 0`
on the desktop runtimes, plus the authenticated relay smoke with one headless
server and two headless clients. A successful compile is not device certification;
use the compatibility table for manual gates.
