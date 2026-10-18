---
name: doriax-safety-rules
description: Hard rules for working in the doriax repo - never launch the editor binary, the single DORIAX_BUILD_DIR build tree, staging discipline with concurrent agents, generated-file boundaries, and what must never be committed. Read this before any build, verification, git or file-deletion action here.
---

# Non-negotiable rules

These exist because of incidents in this repo, not as style preferences.

1. **Never launch `doriax-editor.exe`** (or ask the running one to reload) to verify a
   change. Its autosave previously rewrote a template scene into ~18,870 lines of
   damage that had to be restored with `git checkout HEAD --`. Verify headless
   (`doriax-engine.exe`, `doriax-resource-compiler.exe`, `doriax-build-generator.exe`,
   `ctest` is empty) and, when only the GUI can prove it, **say so and ask the user to
   look**. Do not accept an implicit "yes" as permission to launch it yourself.
2. **Do not kill the user's running editor** without asking. It holds unsaved work.
3. **One build tree.** Its name is `$DORIAX_BUILD_DIR` (default `build`), read by
   `scripts/*.bat`, `.github/workflows/cmake.yml` and generated game projects. Never
   create `build-debug/`, `build-ninja/`, `build-clean/` etc. to dodge a lock - those
   each carried their own CMakeCache and leaked machine-specific paths into committed
   template projects. Two concurrent `cmake --build` runs in the same tree fail with
   `C1041`/PDB-lock errors that look like compile errors; wait instead of branching out.
4. **Never `git add -A` / `git add .`.** This working tree routinely carries another
   agent's in-flight edits (right now: `editor/App.cpp`, `editor/App.h`,
   `editor/main.cpp`, `CMakeLists.txt`, `editor/cli/ResourceServerClient.*`,
   untracked `editor/window/HubWindow.*`, `editor/util/ProcessLaunch.*`,
   `assets/Character/`). Stage explicit file lists, run `git status` after staging, and
   do not commit work you did not write.
5. **Never commit run artifacts.** Ignored already: `editor.log`, `*_stdout.txt`,
   `*_stderr.txt`, `Projects/`, `build-*/`. Not ignored, so exclude by hand:
   `stderr.txt`, `stdout.txt`, `build_editor.log`, and stray
   `editor.log`/`settings.yaml`/`editor_layout.ini` inside `templates/*`. Do not stage
   `templates/charactercontrol-main/BananaMan.glb` or any binary asset without asking.
6. **Never hand-edit generated output**: `build/generated/shaders.h` (from
   `generate_shaders`), `engine_api_suggestions.h` (from
   `generate_api_suggestions.py` / `doriax-reflector`), a project's `CMakeLists.txt`
   and its `.doriax/` directory (regenerate with `doriax-build-generator generate`),
   and `engine/shaders/*.h` base64 stubs. `ProjectBuild.cmake` is the only hand-edited
   build file in a project.
7. **One writer per directory.** Check mtimes before editing; if a file or skill
   appeared mid-session that you did not create, the area is contested - re-read it and
   adapt rather than overwriting.
8. **Reversible beats deleted.** Move/rename/stash rather than `rm` on repo paths, and
   run `git status` before any checkout/reset/clean.
9. **No new third-party dependency** without a strong reason; everything is vendored
   under `engine/libs/` and `libs/` on purpose.
10. **A change is not "done" until** (a) it builds with MSVC (authoritative; MinGW
    accepts things MSVC rejects), (b) an executable actually runs and prints something
    sane, and (c) anything GUI-only is explicitly marked unverified. There are no unit
    tests here.
11. **Skills are mirrored in four trees** - `.qoder/skills/` (Qoder), `.opencode/skills/`
    (OpenCode), `.agents/skills/` (Codex CLI: verified root `C:/doriax/.agents/skills`),
    `.claude/skills/` (Claude Code). Same 20 names in each; if you change a fact in one
    (`file:line` anchor, a "not wired" verdict), apply it to all four.
