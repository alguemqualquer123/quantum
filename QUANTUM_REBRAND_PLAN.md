# Quantum rebrand plan — doriax → Quantum

Renames everything that carries the engine's name to **Quantum**, and replaces
doriax's public links with the fork owner's. Planned in phases; each phase ends
building and running, with its own commits.

## Ground rules (license)

The MIT license this tree ships under has exactly one condition, and it is the
copyright notice:

> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.

So this plan **never** touches:

- `LICENSE` (`Copyright (c) 2026 Eduardo Doria Lima`) — stays byte-identical.
- The `// (c) Eduardo Doria` + `SPDX-License-Identifier: MIT` header in the
  **1384** files that carry it. Renaming the engine does not change who wrote it.
- Third-party notices under `engine/libs/` and `libs/`.

It **adds** `NOTICE.md` + `AUTHORS` (fork of doriax, upstream commit base, © the
fork owner for the modifications) and rewrites the *marketing surface*: README,
logo, docs links, window titles, About box.

## Measured terrain

| Signal | Count |
| --- | --- |
| Tracked files | 6968 |
| Files containing `doriax` (any case) | 1459 |
| Occurrences: `doriax` / `Doriax` / `DORIAx` | 3914 / 1239 / 2254 |
| Paths containing the name | 3003 |
| …of which generated `templates/*/.doriax/` | **2879** (96%) |
| …real source paths | 5 `tools/*/*.cmake`, `tests/doriax_tests.cmake`, `scripts/doriax-build-dir.bat`, `libs/uilab/doriax-patches/*`, skill dirs in 4 trees |
| Files with `(c) Eduardo Doria` | 1384 (untouchable) |
| `quantum` collisions in the tree | 4, all inside vendored `miniaudio.h` in generated engine-api copies |

Two findings that shrink the job:

- **The on-disk formats do not carry the name.** VFS schemes are `asset://`,
  `builtin://`, `data://`, `lua://`, `shader://`; the resource pack magic is
  `DXPK1`. Shipped `.pak`/`.sdat` survive the rename untouched.
- **The Lua API does not carry the name** either (`Engine`, `RegisterEvent`,
  `RegisterEngineEvent`). Existing game scripts keep working. C++ scripts that
  write `doriax::` are covered by the compatibility alias in phase 2.

## Decisions

- **D1 — names.** `quantum` (namespace, targets, lowercase tokens), `Quantum`
  (display), `QUANTUM_` (macros and CMake options). No `Doriax`/`doriax` left in
  new identifiers.
- **D2 — generated output is regenerated, never sed'd.** `templates/*/.doriax/`,
  `build/generated/*`, per-project `CMakeLists.txt` and `AGENTS.md` come from
  `quantum-build-generator` / `editor/Generator.cpp`. Hand-editing them is
  already forbidden by `AGENTS.md`; a rename makes it worse, because the next
  regeneration would reintroduce the old name.
- **D3 — transition aliases, then remove them.** A `doriax_compat.h` with
  `namespace doriax = quantum;` and `#define DORIAX_API QUANTUM_API` &c. keeps
  out-of-tree project code and every template building while phases 2-5 land.
  Phase 7 deletes the aliases once nothing in-tree needs them.
- **D4 — user data migrates instead of vanishing.** `%APPDATA%/Doriax` becomes
  `%APPDATA%/Quantum`; first launch copies `settings.yaml`, `editor_layout.ini`
  and `editor_console.log` if the new file does not exist. Same for the
  per-project `.doriax/` → `.quantum/` directory (moved, not copied; it is
  regenerable anyway).
- **D5 — the fork's own library follows.** `libs/uilab/doriax-patches/` and
  `DORIAX-PATCHES.md` are ours, so they become `quantum-patches/` and
  `QUANTUM-PATCHES.md`, and `scripts/sync-uilab.sh` plus the `doriax_imgui`
  target name move with them.
- **D6 — skills get renamed with the project**, in all four mirrored trees, or
  every future agent session reads a stale name.

## Phases

### Phase 0 — safety net
Checkpoint commit of the current state; record the upstream base commit; write
`NOTICE.md` + `AUTHORS`; add `QUANTUM_REBRAND_PLAN.md` (this file); confirm no
other agent is mid-edit (`engine/core/pool`, `tools/resource-server` were being
worked on today). **Test:** `git log` shows the base; a clean
`cmake --build build --target doriax-editor` succeeds before anything changes.

### Phase 1 — the rename tool, not the rename
`scripts/quantum-rename.sh`: deterministic token rewriter with an explicit
denylist (`engine/libs/`, `libs/imgui/`, `libs/yaml-cpp/`, `libs/box2d/`,
`libs/joltphysics/`, `libs/lua/`, `libs/harfbuzz/`, `libs/soloud/`,
`libs/tinygltf/`, `libs/uilab/third_party/`, every `LICENSE`/`copyright` file,
and any file whose only doriax text is the `(c)` header — the tool must skip the
first two lines of every source file). `--report` mode prints what would change
per layer and asserts the counts above. **Test:** `--report` matches the table;
`--apply` on a scratch clone is `git diff`-reviewable and reversible.

### Phase 2 — code
`namespace doriax` → `namespace quantum` (engine + editor + slate + tools),
`DORIAX_*` macros/options → `QUANTUM_*`, `DORIAX_INIT`, `doriax::editor`,
include guards in `engine/`, and `doriax_compat.h` (D3). Largest phase:
`CMakeLists.txt` (212 hits), `engine/CMakeLists.txt` (119),
`editor/Generator.cpp` (150). **Test:** MSVC builds `quantum` + `quantum-editor`;
`quantum-engine --project templates/<x> --window 1280x720` runs headless and
prints its normal boot log; `ctest` stays empty (there are no tests).

### Phase 3 — artifact names
Targets and executables: `doriax`, `doriax-editor`, `doriax-engine`,
`doriax-reflector`, `doriax-resource-compiler`, `doriax-build-generator`,
`doriax-resource-server`, `doriax-editor-cmd`, `doriax_imgui`; the
`tools/*/*.cmake` files (renamed with `git mv`), `tests/doriax_tests.cmake`,
`scripts/*.bat`, install rules, `doriax_add_windows_resource` /
`doriax_add_windows_manifest`, `.github/workflows/cmake.yml`, and every
sibling-executable lookup (`editor/cli/ResourceServerClient.cpp` searches for
`doriax-resource-server.exe` next to the editor). **Test:** CI-parity build from
a fresh `build/` (the old cache cannot be reused — target names changed), and the
editor finds and spawns the renamed resource server.

### Phase 4 — on-disk state and branding
`%APPDATA%/Doriax` → `%APPDATA%/Quantum` with the D4 migration; `.doriax/` →
`.quantum/` in `editor/Generator.cpp` and the project loaders; window titles
(`formatWindowTitle`), version strings, `doriax-editor.ico` →
`quantum-editor.ico`, About/README logo, and the `Not a doriax resource pack`
error text. **Test:** launch against a copy of a template outside the repo and
check the settings/layout land in the new config dir; `--project` on a project
with an old `.doriax/` still opens it.

### Phase 5 — templates and generated output
`git rm -r` the 2879 committed generated files under `templates/*/.doriax/` and
regenerate them with `quantum-build-generator generate --project <dir>`; update
`ProjectBuild.cmake` and the templates' `AGENTS.md`; add the generated dirs to
`.gitignore` if the regeneration proves them disposable. **Test:** every template
configures and builds; `quantum-engine --project` runs each one.

### Phase 6 — docs and agents
`README.md` (your links, your logo, a "forked from doriax" line that keeps the
attribution), `AGENTS.md`, `CLAUDE.md`, the hard-rules section, and the 21
skills renamed `doriax-*` → `quantum-*` across `.qoder`, `.opencode`, `.agents`
and `.claude` (byte-identical, id = folder name). **Test:** `grep -ri doriax`
over the docs finds only the attribution lines.

### Phase 7 — take out the scaffolding
Delete `doriax_compat.h` and the legacy macros, fix whatever still fails, and
run the final sweep. **Test:** build + the headless runs again; `git grep -i
doriax` returns only `LICENSE`, the per-file `(c) Eduardo Doria` headers,
third-party notices, and the "forked from" lines in `README.md`/`NOTICE.md`.

## Risks and non-goals

- **This is a hard fork.** After phase 2, upstream commits conflict on nearly
  every file. Merging doriax's future work means cherry-picking by hand, or
  keeping a second branch that stays renamed-free. Recorded in `NOTICE.md`.
- Concurrent agents must be paused during phases 1-3; a repo-wide rewriter
  running against a tree others are editing produces conflicts nobody can read.
- One build tree only (`$DORIAX_BUILD_DIR`, renamed to `$QUANTUM_BUILD_DIR` in
  phase 3): the existing cache must be deleted, not reused.
- Out of scope: touching `LICENSE` or the per-file copyright headers, renaming
  third-party code, and any change to shipped binary formats (none is needed —
  the magic bytes and VFS schemes do not carry the name).
