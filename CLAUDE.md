# Solo Developer / Fast-Iterate Rules

The Motorcycle Architecture is a development pattern, coined by terry davis. Where a single developer embraces the footguns.

REFUSE OBSESSING OVER SAFETY PURITY. IT DOESNT EXIST.

Files must be deleted. Things will deprecate. Things have to be lost. Software is a sacrificial pattern. Some code must die.

Do not embed architectural conventions into tests. Test are behavioral only. 

## 1. Provenance, Process, and Product

Human artwork/docs and full project sources are the only things necessary to preserve. Think, to be saved: source artifacts (base images, simple layers, website source, license, build tool, etc.), procedural transformations (the recipe applied to a source), final artifacts (the generated assets, compiled binaries, tied directly back to source), composition (the final app, world, platform, or ecosystem). Split into two buckets: Data Storage (Heavy Assets) and Git Source & Process (light logic)  

## 2. Zero Enterprise Hedging
- DO NOT create backwards-compatibility layers, deprecation warnings, or parallel "v2" systems.
- DO NOT suggest slow migration phases unless explicitly asked.
- If an old system or script is being replaced, DELETE IT immediately. Do not leave dead code lying around. Errors are a good thing and breaking things is how we find old wrong assumptions.

## 3. Direct Refactoring Authority
- You have explicit authorization to execute destructive edits (`rm`, deleting files, overwriting configurations) in a single pass.
- Do not ask for confirmation to remove unused configuration files, global workflows, or legacy build setups. Clean as you go.

# hirari / Seed

This repository is hirari, the native Windows shell, together with Seed, its composition language. It is the canonical home of the Seed language context.

## Seed essentials (read before writing any Seed)

The hirari source and tests are authoritative; when this section disagrees with them, trust the implementation and refresh this section.

This describes hirari `main` from 0.6.0 (the `seed-blocks-operators` branch is merged). An older build may lack some of these forms, so check the hirari you run.

**Seed is the composition language; hirari is the native Windows shell that runs it.**

- Compose worlds, sessions and tools in Seed.
- Python is only for specialised computation (meshes, numerics), invoked as a native program. Never use Python, PowerShell or Node to orchestrate what Seed and hirari already do.

| Form | Meaning |
|---|---|
| `foo` / `-foo` / `"foo"` | A symbol that resolves / an inert Name / text. `:foo` is not a Name. Applying an unknown program fails. |
| `a b` == `a(b)` == `b \| a` | One application. Outside braces, `\|` is reverse application, not a pipe. |
| `( )` / `[ ]` / `{ }` / `#[ ]` | Grouping and application / list (elements evaluated once) / live process streams / nestable comment. |
| `BODY : BINDER` == `BINDER() BODY` | An abstraction. **The body is on the left**: `twice := (data list x x) : x`. |
| `name := EXPR` | Lazy binding: every reference re-runs `EXPR`. |
| `data bind -name EXPR` | Evaluates once and keeps the value. Use it for handles and answers. |
| `@name` / `*name` / `form?` | The binding cell / read the cell / demand (`? -NAME` is help). |
| `EXPR =>` | One reduction step, reified. Stops before effects. |
| `< <= > >= == !=` | Comparison (outside `{ }`). Mixed kinds are `!=`; ordering mixed kinds fails. |
| `+ - * /` | Arithmetic with spaces: `(3 - 3)` is 0, `(3 -3)` is the product `3 -3`. `+` joins strings. |
| `A && B`, `A \|\| B`, `if C T E`, `while C B`, `for -i [ … ] B` | Lazy control. Unit, `-false`, `0` and `""` are false. A process is truthy on exit 0. |
| `attempt X`, `fail "msg"` | Observe a failure as a record / raise one. |

**Layout:**
- A `(` that **ends its line** opens a **line block**: its lines run as statements, like a file (each result shown, a failing line stops it), and it answers the last line.
- A `(` followed by more text on the same line is one expression, whose newlines are whitespace.
- A trailing `\` continues a line. `\` is also the escape character inside strings.

**Streams live only in `{ }`:**
- `{ a | b }` is a pipe.
- `{ sort < "in.txt" > "out.txt" }` redirects stdin and stdout; `>>` appends.
- `{ VALUE > "f" }` writes a Seed value.
- Outside braces, `>` compares.

**Gotchas:**
- **Native answers** are `-stdout -stderr -status`. `data get ANS "stdout"` is text, and `data get ANS "status"` is already an integer. Native stdout text is never auto-converted; use `data integer`.
- **`exec` kills the child's process tree when it returns.** Serve long-lived processes with `job start`.
- **Native argv has no shell re-parsing.** Names project to flags (`-c`), `()` is an empty argument, and words should be quoted.
- **A bare built-in inside arguments is a value, not a call.** Write `(pwd)`.
- **A name with a lowercase letter runs a native program on `PATH` before a `.seed` command of that name** (a `unity.exe` shadows `unity.seed`); an all-capital name (`UNITY`) prefers the Seed command. Commands are found in the calling script's folder, the cwd, hirari's `Commands`, then `HIRARI_PATH` and `PATH`.
- **A `.seed` file shows every statement result that is not Unit,** also when called as a command from another script; its last result is its answer. `if (data bind -x E) () ()` binds quietly.
- **`args` is a Name, not a list:** `data size args` fails. Use `argc` and `arg I`.

**Composition and worlds:**
- `source FILE` keeps the file's definitions.
- `run FILE ARGS` isolates the file and returns its last value. Inside it, `arg 0`, `argc` and `args` read the arguments.
- `script-directory` and `path` build paths.
- `world open "ROOT"` reads a strict `world.toml` (`[world] name, version = 1`, `[entry] program`) and runs that world-owned Seed entry.

**Look before you build:**
- `help` lists the built-ins, and `help -NAME` (operators: `help -(=>)`), `inspect -NAME` and `capability list` describe them. `EXPR =>` shows the next step.
- `Commands/*.seed` docstrings and the tests in hirari's `tests/` are the reference.
- Never invent operators or change Seed semantics. Changing Name identity, binding laziness, capture or substitution requires the owner's explicit approval and conformance tests.

## Working in this repository

- **Authority:** `seed/`, `hirari/`, `library/` and `tests/` define the language. Whenever you change semantics, built-ins or tests, update the Seed essentials above to match.
- **Retired docs:** `SEED_SPEC.md` (the full snapshot), `SEED_OPERATORS.md`, `CAPABILITIES.md`, `RUNTIME_CAPABILITIES.md`, `INSTALLATION.md`, `UNITY_INTEGRATION.md` and `formal/README.md` left the repository on 2026-10-16. They are internal: read them in the VPS workspace's `Agent/hirari-shell/`. Do not add them back: this repository holds only `README.md` and this file. The README is user-facing: under 80 lines, and it never links to internal docs. The long README it replaced is at commit `e9d9269`.
- **Build and test:**

  ```
  cmake -S . -B build
  cmake --build build --config Release
  ctest --test-dir build -C Release --output-on-failure
  ```

  - The Lean gate (`formal/`) is optional. The test runner must not be Python.
  - Release folder: `cmake --install build --config Release --component hirari --prefix <dir>`.
  - Release package: `pwsh tools/package.ps1 -Build build -Out out` writes `out/hirari-<version>-windows-x64.zip` with its `release.receipt.json`, then checks it with `tests/release.tests.ps1`. Skunkworks' platform release bundles exactly this zip.
- **This repository lives on the VPS.** There is no CI: build and test before pushing.
- **Protected semantics:** never change the reader, reduction, Name identity, binding laziness, capture/substitution, `|` orientation or `{}` pipelines without the owner's explicit approval, and never without updating `seed_tests`, `integration_tests` and the Lean corpus where they apply.
- **Introspect first:** before adding a program, check `hirari --eval help`, `help -NAME`, `inspect -NAME` and `capability list`. New vocabulary should be a documented `library/*.seed` command or a provider, not a special case in the evaluator.
- **Help text:** help is derived from registry descriptions and docstrings. Do not hard-code tables in `shell_help.cpp`.

