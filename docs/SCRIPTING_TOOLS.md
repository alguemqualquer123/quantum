# Scripting tools

This slice adds a headless Lua debug session and a small, deterministic graph
asset contract. It is intentionally embeddable: it does not add a debugger
library or a second scripting runtime.

## Lua debugger

`LuaDebugger` runs a chunk on a Lua 5.4 coroutine and uses the standard Lua hook
API. It supports source breakpoints, continue, step into, step over, step out,
call-stack snapshots, simple watches while paused, and structured runtime
diagnostics with a traceback.

The editor command-line twin exposes it without opening a window:

```text
quantum-editor-cmd script debug --file scripts/player.lua \
    --breakpoint scripts/player.lua:12 \
    --commands "watch speed;stack;next;continue"
```

Um breakpoint pode receber uma expressão condicional com `--condition`; ela é
avaliada no frame atual, com acesso às variáveis locais e ao ambiente global:

```text
quantum-editor-cmd script debug --file scripts/player.lua \
    --breakpoint scripts/player.lua:12 \
    --condition "speed > 10" --commands "continue"
```

For adapters that need machine-readable results, append `--json`. The command
then emits exactly one JSON object to stdout after the session ends; debugger
events are not mixed into that stream. The object always contains `state`,
`stopReason`, `reason`, `diagnostic`, `watches` and `callStack` (and a
`commandErrors` array for commands that were not recognized):

```text
quantum-editor-cmd script debug --file scripts/player.lua \
    --breakpoint scripts/player.lua:12 \
    --commands "watch speed;next;continue" --json
```

Antes de abrir o editor, o lint headless valida a sintaxe Lua e aponta chamadas
de APIs removidas ou incompatíveis com Lua 5.4. Os diagnósticos JSON são
determinísticos; `--strict` transforma warnings em falha de CI:

```text
quantum-editor-cmd script lint --file scripts/player.lua --json
quantum-editor-cmd script lint --file scripts/player.lua --json --strict
```

`diagnostic` contains `message`, `source`, `line` and `traceback`. Each entry in
`watches` contains `expression` and `ok`, plus either `value` or `error`.
`callStack` is an array of frames with `level`, `source`, `name`, `what`,
`line` and `currentLine`; it is empty when the completed session has no active
frame. `stopReason` is the debugger reason (`breakpoint`, `step`, `pause`,
`error` or `none`), while `reason` describes why the CLI session ended, such as
`completed`, `error`, `quit` or `input_eof`.

For an IDE adapter, use the line protocol:

```text
quantum-editor-cmd script debug --file scripts/player.lua \
    --breakpoint scripts/player.lua:12 --protocol
```

Commands are `continue`, `step`, `next`, `out`, `watch <expression>`, `stack`
and `quit`. The protocol is deliberately line-oriented so an editor adapter can
own transport and source-map policy without coupling the engine to one IDE. With
`--protocol --json`, commands are read from stdin as before, but the `ready` and
intermediate text events are suppressed and the same final JSON object is emitted
on EOF, `quit` or a terminal debugger state.

For a local IDE adapter that cannot own the child process stdin, the same
line protocol can be served on loopback:

```text
quantum-editor-cmd script debug --file scripts/player.lua \
    --breakpoint scripts/player.lua:12 --protocol \
    --tcp-loopback 0 --tcp-timeout-ms 30000
```

The command prints the selected port to stderr, accepts one client on
`127.0.0.1`, enforces a bounded line size and closes the socket on every exit.
For a live exported/runtime process, `quantum-engine` exposes the same style of
line protocol before it loads the project entry:

```text
quantum-engine --project build/game --debug-port 0 \
    --debug-token-env QUANTUM_DEBUG_TOKEN
```

The runner prints the selected loopback port, sends a JSON `hello` event with
protocol version `2`, and requires `auth TOKEN` when a token was configured.
After authentication it
accepts `status`, `break <source>:<line> [if <expression>]`, `delete <id>` or
`clear`, `continue`, `pause`, `step`, `next`, `out`, `watch <expression>`,
`stack`, `errors on|off|all|none|<categories>`, `ping` and `quit`. The endpoint accepts one client at a time, keeps
the listener on `127.0.0.1` only, bounds every input line to 64 KiB and does not
put the secret in a child process argument unless the caller explicitly uses
`--debug-token` instead of `--debug-token-env`.

The live hook is cooperative: a callback is allowed to unwind, then game events
pause while drawing and the debugger service continue. Locals and upvalues are
snapshotted at the stop point, so watches remain valid after the Lua callback
returns. `scripts/remote-debug-smoke.py` validates authentication, conditional
breakpoints, lexical watches, stepping and continue against a real headless
`quantum-engine` process. The `View > Script Debugger` panel keeps the
`Play (in-process)` tab and now also exposes a `Remote TCP` tab for the same
loopback endpoint; it owns a non-blocking connection, authentication, polling,
breakpoints, stepping, watches and stack refresh without blocking the editor.

Protocol version 2 adds optional request correlation without breaking version 1
clients: send `@<id> <command>` (for example `@build-17 status`) and the JSON
response echoes the same `requestId`. IDs are limited to 64 ASCII letters,
digits, `-`, `_` and `.` characters; they are opaque to the runtime and can be
preserved by an IDE across reconnects.

The error filter is category-based. `errors on` restores the default
`runtime,callback` categories and `errors off` disables stopping; an exact list
such as `errors runtime,syntax`, `errors all` or `errors none` selects the
categories explicitly. `+runtime` and `-callback` update the current mask
incrementally. The server reports `errorFilter` and `errorMask` in `status` and
`errorFilter` events. Runtime failures from protected script calls and callback
failures are classified separately; syntax is reserved for load/compile errors.

The standalone watch evaluator runs in the suspended coroutine. The attached
and remote modes evaluate against the captured locals/upvalues plus the global
environment and should be treated as read-only by the adapter; Lua itself
cannot enforce that policy. Native userdata is reported by type, not
introspected.

The CLI debugger executes an isolated Lua chunk and does not preload engine
globals such as `Scene` or project modules. Use `quantum-engine --script-smoke`
or the editor Play debugger for code that requires the live engine API.

## Visual scripting round-trip

`editor/graph/GraphAsset` defines the first YAML contract for a graph: version,
name, nodes, typed directional pins, positions and edges. The headless commands
validate references, duplicate IDs and pin direction, then serialize a canonical
YAML form and reload it:

```text
quantum-editor-cmd script graph-validate --file assets/Scripts/Player.graph
quantum-editor-cmd script graph-roundtrip --file assets/Scripts/Player.graph \
    --out .quantum/graphs/Player.roundtrip.graph
```

The runtime now prepares built-in graph nodes through the same `ExecutableGraph`
path used by Play. The headless gate catches a graph that passes the authoring
schema but has no registered runtime handler:

```text
quantum-resource-compiler graph \
    --validate-runtime assets/Scripts/Player.graph --json
quantum-resource-compiler graph \
    --validate-runtime .quantum/graphs/Player.gdat
```

The standalone runner also executes a small graph through the real runtime
interpreter, including a pure math dependency and a variable write:

```text
quantum-engine --headless --graph-smoke --frames 1 --warmup 0
```

The smoke must report `result=7.500` and exits non-zero on schema, preparation or
execution failure.

The current format intentionally preserves only the v1 data model; unknown
future node fields are not retained yet. Reflected entity methods can be called
by the graph runtime through the live Lua binding:

```text
method.<Class>.<method>
```

The editor also exposes a `Call Reflected Method` node where the class, method
and argument count are editable. The adapter accepts entity handles and the
primitive/vector graph values supported by the Lua binding, and writes a
primitive return value to `result`. Static methods, C++-only methods, object
returns and methods that are not registered in Lua remain unsupported.

The reflection artifact now has a headless consumer as well:

```text
quantum-resource-compiler graph \
    --catalog-file build/generated/engine_node_catalog.json --json
```

It validates the generated catalog version, unique classes, method signatures
and member arrays before packaging. It reports deterministic counts. The
`--validate-runtime` gate covers built-in nodes and the reflected method adapter
when the graph contains a `method.<Class>.<method>` node.

The runtime project smoke closes the load path as well as the in-memory path:

```powershell
build\Release\quantum-engine.exe --project templates/daynight --headless `
  --graph-smoke --graph-project-smoke --frames 1 --warmup 0
```

`--graph-project-smoke` writes a temporary `.gdat` under the selected project's
asset root, loads it through `asset://` and executes it through `GraphBinding`.
The temporary asset is removed before the process exits, so the source project
is not modified. The Release smoke currently exercises `Object.setVisibleOnly`
and reports `asset:// load, reflected method, result=7.500`.

## C++ hot reload

During a C++ Play session, **Scene > Reload C++ Scripts** starts an asynchronous
project rebuild. The editor stages the candidate DLL while the old library stays
loaded, validates `quantumProjectAbi`, pauses events, cleans subscriptions on
the owner thread, swaps the library copy and reinitializes the live scenes.
Compilation or ABI failures preserve the old DLL, and the prior Play pause state
and attached Lua debugger are restored. New source files and generated
project structure still require restarting Play so the generator can refresh the
source list.

## Limitations

- The debug session is coroutine-based. Existing engine callbacks continue to
  use the normal `LuaBinding` state and are not paused by this CLI session.
- Conditional breakpoints evaluate Lua expressions with the current locals and
  globals, read-only by convention. The category-based `errors` filter controls
  whether protected runtime or callback Lua errors pause the attached debugger;
  TCP transport is
  loopback only and is not an editor Play
  endpoint.
- Watches can execute arbitrary Lua for non-identifier expressions.
- Reflected graph calls require a registered Lua entity method and a valid
  target entity. They do not yet expose static/system methods, C++-only methods
  or object-valued returns.
- The clean base branch uses the Quantum names throughout; integration into
  a rebranded `quantum` branch is a mechanical rename outside this slice.
