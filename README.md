# hirari

hirari is a native Windows shell that runs Seed, a composition language. Seed
owns values, names, bindings and evaluation. hirari owns program resolution,
live process pipelines, interactive terminal programs and optional typed
providers. It is a general-purpose shell; no rendering engine is required.
Pre-alpha builds for Windows x64 are at https://emergencetech.co.

## Build

You need CMake 3.24 or newer and a Windows C++23 toolchain with ConPTY.

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/Release/hirari.exe
```

`cmake --install build --config Release --component hirari --prefix <dir>`
writes a release folder. An installed shell needs no development toolchain.

## Run

- `hirari` starts the interactive shell; `exit` or `quit` ends it.
- `hirari --eval SOURCE` evaluates one expression, and `hirari --file PATH` runs a UTF-8 script.
- Source errors report the file and line, stop execution and return nonzero.

## Seed at a glance

| Form | Meaning |
| --- | --- |
| `foo` | Resolve a binding or a host program. |
| `-foo` | A Name: the identity of foo. |
| `"foo"` | Text. |
| `a b`, `a(b)`, `b \| a` | The same application, in three orientations. |
| `greeting := "hello"` | Bind an expression; each use re-runs it. |
| `data bind -name EXPR` | Evaluate once and keep the value. |
| `[1 2 3]` | A list; each element evaluated once. |
| `3 - 3`, `a < b` | Arithmetic and comparison, written with spaces. |
| `expr =>` | One reduction step, without running effects. |
| `{ a \| b > "out" }` | Live process streams: `\|`, `<`, `>`, `>>`. |
| `(` at line end | A line block: its lines run as statements. |
| `#`, `#[ ]` | Line and nestable bracket comments. |

```text
greeting := "hello"
if -true "yes" "no"
for -item -("alpha" "beta") item
git "status"
{ python -c "print('z'); print('a')" | sort }
```

`help`, `help -NAME`, `inspect -NAME` and `capability list` describe every built-in.

## Worlds and background work

`world open "path/to/world"` reads the folder's `world.toml` and runs its
entry program. A failed entry restores the previous world, working directory
and environment.

```toml
[world]
name = "Example world"
version = 1

[entry]
program = "start.seed"
```

`job` starts native programs in the background, with progress, cancellation,
timeouts and results.

## License

Copyright (C) 2026 Jinkai. hirari and Seed are licensed under the GNU General
Public License, version 2 only (`GPL-2.0-only`); see [LICENSE](LICENSE). PCRE2 is BSD-3-Clause with the
PCRE2 exception, and nlohmann/json is MIT. [NOTICE](NOTICE) states what the
license covers, and a release installs the third-party licenses under `Licenses/`.
