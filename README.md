# Quantum Engine

Quantum is an MIT-licensed C++17 game engine and visual editor maintained by SR VINIX.
It provides an ECS runtime, 2D/3D rendering, Lua and C++ scripting, physics, audio,
networking, asset validation, and native project export.

## Current release

**v0.8.2** — the current stable release of the Quantum engine.

- Engine repository: <https://github.com/alguemqualquer123/quantum>
- Documentation: <https://alguemqualquer123.github.io/quantum-docs/>
- Documentation repository: <https://github.com/alguemqualquer123/quantum-docs>

The `main` branch contains the next development state. Use the `v0.8.2` tag when
you need a reproducible engine checkout.

## Build

Quantum uses CMake and supports the configured native graphics backends for each
platform. On Windows, the authoritative CI toolchain is MSVC:

```powershell
cmake -S . -B build
cmake --build build --config Release --target quantum-editor
```

The editor command-line tools can validate projects and assets without opening the
GUI:

```powershell
build\Release\quantum-editor-cmd.exe --help
build\Release\quantum-editor-cmd.exe doctor --project templates\daynight
```

See the documentation for installation, editor workflows, scripting, networking,
rendering, asset pipelines, and release packaging.

## License

Quantum is distributed under the [MIT License](LICENSE). Vendored dependencies and
third-party assets retain their own licenses.
