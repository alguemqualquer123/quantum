# Suite port — C:\quantum → C:\quantum2

## Goal

Move the work built in `C:\quantum` onto the upstream-synced checkout `C:\quantum2`
(`origin/main` @ `7d197c71`, 333 commits ahead of the shared base: water, scene pipeline,
render passes, `engine/tools/shaderlib` → `engine/shaderlib`). Nothing new is designed
here: the target line must end up with upstream's engine **plus** our suite work.

Carried over: the five standalone executables (`quantum-reflector`,
`quantum-resource-compiler`, `quantum-build-generator`, `quantum-engine`,
`quantum-resource-server` with its connections window and tray life), `editor/cli/`,
the vendored `libs/uilab` + `quantum-patches/` + `scripts/sync-uilab.sh` and the editor
chrome routed through it, ImGui 1.93.0-docking, the token-driven theme, the Hub/welcome
and template flows, `engine/slate`, the BananaMan builtin model, the single
`QUANTUM_BUILD_DIR` tree, the missing-asset anticrash, `AGENTS.md` + the four mirrored
skill trees, and the plan/roadmap documents.

## Mechanism

`git merge --no-commit --no-ff` of `C:\quantum` main into branch `suite-port` (created from
`7d197c71`), not a file copy — upstream touched the same pools, bindings, `App.cpp` and
`CMakeLists.txt` we did, so every shared file needs a real 3-way result.

## State

| bucket | count | status |
|---|---|---|
| added cleanly from our side (`tools/`, `libs/uilab/`, `templates/`, skills, `engine/slate`) | 3269 | staged |
| auto-merged shared files | 63 | staged, needs build proof |
| `engine/shaderlib/*` relocations (dir renamed upstream) | 34 | resolved |
| content conflicts | 34 | open |

## Phases

0. Branch + merge opened. **Done** (`suite-port`, MERGE_HEAD set).
1. Resolve the 34 content conflicts, partitioned so no two writers share a file:
   - engine: `engine/CMakeLists.txt`, `engine/core/**` (13 files)
   - editor chrome: `editor/App.{cpp,h}`, `EditorFrame.cpp`, `main.cpp`, `Exporter.cpp`,
     `Generator.cpp`, `ai/AiService.cpp`
   - editor panels/commands/backends: `editor/window/**`, `editor/command/type/**`,
     `editor/render/SceneRender2D.cpp`, `editor/backend/**`
   - repo level: `CMakeLists.txt`, `README.md`, `.github/workflows/cmake.yml`
2. Re-wire what the merge cannot know: `QUANTUM_EDITOR_SOURCES` and `tools/*/*.cmake`
   include lines, the `engine/shaderlib` path in `generate_shaders.cmake`, the reflector
   `check` gate in CI, the `quantum-patches` series against the new ImGui.
3. Configure + build in the tree `QUANTUM_BUILD_DIR` names (`C:\quantum2\build`,
   "Visual Studio 18 2026"): `quantum`, then the five tools, then `quantum-editor`.
   One builder at a time (shared PDB).
4. Headless verification: `quantum-reflector check` (staleness gate),
   `generate_shaders`, `quantum-resource-compiler pak|shaders|inspect`,
   `quantum-resource-server --once` then `--port 0` + `connections` JSON,
   `quantum-engine --project <scratch>` with the Lua anticrash harness
   (`C:\Users\Admin\anticrash-lua`). The editor is **never** launched by an agent.
5. Sync the four skill trees + `AGENTS.md` to whatever actually ended up in the tree,
   then report and wait for a commit/push decision.

## Invariants

- Both sides survive: never drop an upstream feature to make a conflict disappear, never
  drop our suite/UILab/anticrash behaviour to make it compile.
- Asset build threads report failure as a value (`TextureBuildResult` / `SoundBuildResult`
  → `ResourceLoadState::Failed` + `errorMessage`); a throw escaping a pooled task aborts
  the process because `_HAS_EXCEPTIONS=0` leaves `std::packaged_task` with no slot.
- `libs/uilab` is pristine upstream + `quantum-patches/`: edit it only through
  `scripts/sync-uilab.sh regen`.
- Generated files (`build/generated/shaders.h`, `engine_api_suggestions.h`, a project's
  `CMakeLists.txt`/`.quantum/`) are never hand-edited; new `ComponentType` values are
  appended.
- One build tree per checkout, named by `QUANTUM_BUILD_DIR`. No `git add -A`, no commits of
  run artifacts, no push without a yes.

## Rollback

`git merge --abort` in `C:\quantum2` returns the checkout to clean `7d197c71`; the port
lives only on branch `suite-port`. `C:\quantum` is read-only for this work and keeps its own
in-flight merge untouched.
