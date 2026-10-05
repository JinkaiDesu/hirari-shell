# Hirari

Hirari is an independently runnable Windows shell over the Seed evaluator.
Seed owns values, identities, bindings, products and evaluation. Hirari owns
program resolution, live process composition, foreground terminal programs and
optional access to a running Skunkworks Unity player. Hirari starts and remains
useful without Unity or a world installed.

## Build and run

Developer prerequisites are CMake and a Windows C++ toolchain supporting C++23
and ConPTY. An installed shell uses its compiled executable; it does not need
the development toolchain. MSVC builds use the static runtime: `/MT` for Release,
`/MTd` for Debug. External programs resolve in the child environment's PATH order, including when a bundled tool has the same name as a Windows system program. Redirected child output retains its original bytes.
Standalone Hirari inherits its host environment; Skunkworks supplies the bundled
tools and installation-specific environment for its child shell.

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/Release/hirari.exe
```

```text
hirari>
```

Use `hirari --eval SOURCE` for one expression or `hirari --file PATH` for a UTF-8
script. A script contains one expression per completed line; grouped or quoted
expressions may span lines. UTF-8 BOMs and Unicode paths are supported. Source
errors report the file and line, stop execution and return nonzero. No extra
script language or comment syntax is introduced. Foreground interactive tools
use the interactive shell.

## Seed and shell semantics

These are different values and operations:

| Form | Meaning |
| --- | --- |
| `foo` | Resolve a symbol/binding or a host program. |
| `:foo` | Name: semantic identity of foo. |
| `"foo"` | Literal text. |
| `()` | Empty value. |
| `greeting = "hello"` | Bind the RHS form to greeting. |

Names are not converted to literals in the evaluator. Native `=` retains the
RHS program form; it does not memoize a foreign allocation. Demanding a symbol
reduces its current binding. The existing Program ABI carries an implementation
and opaque host context without adding evaluator value kinds.

```text
greeting = "hello"
greeting
if :true "yes" "no"
if :false "yes" "no"
item = "previous"
for :item :("alpha" "beta") item
item
```

`if` evaluates only the chosen branch. Foreign process conditions use exit
status. `for` restores the previous binding, including when a body fails.

Programs resolve directly; ordinary OS program arguments cross the argv/text
boundary. For tools available in the active environment:

```text
git "status"
python -c "print('hello')"
python -c "print('z'); print('a')" | sort
```

`consumer producer`, `consumer(producer)` and `producer | consumer` compose
programs; pipes run live. Interactive foreground programs use ConPTY with raw
input forwarding, resize and interrupt support. `exit` and `quit` end the shell.

## Unity API

The native persistent named-pipe client uses the canonical generated
`hirari/wire_generated.hpp`, supplied by Skunkworks' protocol schema. It sends a
versioned Hello and accepts Ready with `skunkworks.wire/1` and a session GUID.
`SKUNK_UNITY_PIPE` selects a local instance-specific pipe name; the fallback is
`skunkworks.unity`. No Unity connection is required for standalone shell use.

Available commands are `unity connect/status/disconnect`, `find`, `type`,
`component`, `get`, `set`, `call`, `new`, `release`, `batch` and `at`. Exact and
generic CLR signatures use `new-exact`, `call-exact`, `call-generic` and
`call-generic-exact`. `i32`, `i64`, `f32`, `f64`, `bytes` and `string` are explicit
wire-value helpers. There is no intermediate Runtime executable.

Use **explicit capture** for a reusable foreign value:

```text
unity bind :t (unity type "UnityEngine.GameObject")
unity bind :go (unity new t "GenericProof")
unity set go "name" "RenamedProof"
unity get go "name"
unity call go "SetActive" :false
unity release go
```

`unity bind :destination expression` evaluates the expression once and binds
the returned value. In contrast, `go = unity new t "GenericProof"` keeps the
ordinary lazy Seed form and runs New again each time go is demanded.
Object/type references have typed, opaque host payloads and Name identities;
they are not strings or numeric handles. Numeric and byte results also retain
their exact wire kind. Released and stale-session references are rejected.
A broker Fault leaves the connection usable; transport failures disconnect.
Failed operations are never automatically replayed.

See [Unity integration contract](UNITY_INTEGRATION.md) for exact signatures,
batches, assembly loading, framing and error behavior.

## Generic worlds

`world open "path/to/world"` selects an ordinary project root and runs its
entrypoint. `world status` reports the current selection. No project-specific
world content is part of Hirari.

```toml
[world]
name = "Example world"
version = 1

[entry]
program = "start.seed"
```

The descriptor is a strict minimal v1 schema. The entry must be a file inside
the canonical root. Opening sets the working directory and `SKUNK_WORLD` for
child processes. Validation/entry failures restore the previous world selection,
working directory and environment. Already performed entry side effects remain
owned by that entry. Full descriptor rules are in the integration contract.

## Embedding API

`Host` receives stdout/stderr, interruption and terminal capability callbacks.
`Shell::execute(source)` returns a Seed value, or null with a host diagnostic on
error. `Shell::execute_file(path)` runs expressions in the same evaluator and
returns success/failure. `Shell::status()` exposes the last execution status;
`seed()`, `unity()` and `worlds()` permit host inspection.

`UnityClient` owns one persistent local pipe and serial request/response traffic.
Its connection handshake establishes the server session. `UnityProgram` maps
reflection vocabulary to that codec and owns typed `ForeignRef` Program contexts.
`WorldHost` handles descriptor validation, selected root and launch rollback.
Unity server reflection continues to run through the player's main-thread queue.

## Current regression suite

CTest registers five targets:

| Test | Coverage |
| --- | --- |
| `seed_semantics` | Existing Seed reader/printer/value/Program regressions. |
| `hirari_integration` | Wire golden bytes and all value kinds; partial I/O; persistent Hello/Ready; IDs and frame bounds; faults, disconnect, timeout and no replay; typed/opaque refs, capture identity, release, session guards, CLR signatures and batches; neutral world/script validation and rollback; child environment; Unicode argv/files; control forms; live pipes; exit/quit. |
| `hirari_eval_name` | CLI preserves a Name. |
| `hirari_eval_failure` | Missing program returns nonzero. |
| `hirari_cli_usage` | Invalid CLI options return nonzero. |

The native integration tests compile their own process helper and named-pipe
peer. Their worlds are temporary neutral fixtures, and PATH is restricted to
test binaries plus Windows system programs. They do not rely on developer
Python/Git/Bash installations. The fixture uses binary stdout for exact byte
assertions; production process output remains unchanged. These tests do not
claim actual Unity-player, bundled-tool or clean-machine installer acceptance;
those are Skunkworks package tests.

Release verification on 2026-10-04: all five CTest targets passed. The executable
dependency inspection listed `KERNEL32.dll` only. Re-run CTest for each release;
this result is a source-state checkpoint, not a replacement for release checks.
