// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

# Editor stability diagnostics

This note records the editor failures investigated on Windows and the recovery
behaviour added for them. The source tree uses the Quantum identity.
identifiers; installed builds may expose the same executable as Quantum.

## Evidence

The Windows Application Error events from 2026-11-09 showed two recurring
patterns:

- `quantum-editor.exe` faulted with `0xc0000005` in `quantum.dll`.
  The editor crash log resolved one instance to `RenderSystem::updateCamera` and
  another to `ComponentArray<Transform>::findIndex` while constructing the
  preview camera after a malformed scene path.
- `quantum-editor.exe` and the serialization smoke executable repeatedly ended
  with `0xc0000374` in `ntdll.dll`, consistent with heap corruption being
  detected after an earlier invalid write rather than with an `ntdll` defect.

The same editor log also recorded a duplicated scene path, stale generated model
entity references, invalid image-data validation messages, and a rejected plugin
with ABI `999`. Those are independent triggers, so boot recovery must not delete
the project or silently overwrite its layout.

## Recovery behaviour

The editor writes `.editor-session-active` next to its settings while running and
removes it only during an orderly shutdown. If the marker is present on the next
startup, the editor:

- enters recovery mode;
- preserves and skips the last opened project;
- leaves `editor_layout.ini` untouched but does not ask ImGui to parse it;
- creates a temporary `Recovery Project` so diagnostics can continue.

The same path can be requested explicitly:

```text
quantum-editor.exe --safe-mode
quantum-editor.exe --safe-mode --project C:\path\to\project
quantum-editor.exe --reset-layout
```

`--reset-layout` keeps the normal project selection but rebuilds the dock layout
without reading the persisted ImGui file. Neither option deletes user data.

Settings values loaded from YAML are bounded before they reach the window and
layout code. A malformed settings file is discarded in memory for that session;
the original file is not overwritten by this recovery path.

## Heap diagnostics and ABI follow-up

Windows CRT assertions, errors, and warnings are routed to stderr instead of a
modal dialog, and critical-error dialogs are disabled. WER and the normal crash
handler remain enabled, so a dump can still be collected without hanging a
headless smoke test.

Generated C++ script libraries now export `quantumProjectAbi`. The editor calls
that C function immediately after loading the staged copy and compares the
compiler, pointer width, iterator-debug level, CRT mode, editor layout and
shared-library flags against its own `projectAbiFingerprint()`. Missing or
mismatched signatures are rejected before `initScripts`, preventing a stale or
Debug/Release-incompatible DLL from crossing the engine boundary.

## Headless verification

Run these commands from the isolated worktree, serially:

```powershell
cmake -S C:\qs -B C:\qs\build
cmake --build C:\qs\build --target quantum-editor-cmd --config Debug -- /m:1
cmake --build C:\qs\build --target quantum-editor --config Debug -- /m:1
C:\qs\build\Debug\quantum-editor-cmd.exe --help
C:\qs\build\Debug\quantum-editor.exe --help
```

The GUI executable is not launched as part of validation. To inspect a new
failure, check `%APPDATA%\Quantum\editor.log`, Windows Application Error events,
and `%LOCALAPPDATA%\CrashDumps\quantum-editor.exe.*.dmp`.

The recovery marker is intentionally retained after a crash. A successful normal
editor shutdown clears it; removing it manually is safe only after confirming no
editor process is still running.
