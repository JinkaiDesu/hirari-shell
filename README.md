# hirari

'hirari' is a Windows Shell running 'Seed, the language'.
Seed owns values, identities, bindings, products and evaluation. Hirari owns
program resolution, live process composition, foreground terminal programs and
optional typed providers. It is a general-purpose shell; no rendering engine
is required.

## Build and run

Developer prerequisites are CMake 3.24 or newer and a Windows C++ toolchain supporting C++23
and ConPTY. An installed shell uses its compiled executable; it does not need
the development toolchain. MSVC builds use the static runtime: `/MT` for Release,
`/MTd` for Debug. External programs resolve in the child environment's PATH order, including when a bundled tool has the same name as a Windows system program. Redirected child output retains its original bytes.
Standalone Hirari inherits its host environment; Skunkworks supplies the bundled
tools and installation-specific environment for its child shell.

An embedding host can set `HIRARI_STARTUP` to an explicit `.seed` file. Interactive
Hirari evaluates it once before the prompt in the same session; definitions and
world context remain live. Failure prints a diagnostic and retains the shell.
Standalone Hirari has no implicit application boot script. `--file` and stream-stage
invocations do not run this interactive hook.

The embedding launcher owns startup selection. Its supervisor may also supply
a fresh `HIRARI_STARTUP_STATUS` file path for each shell generation; Hirari
atomically publishes `ready\n` or `failed\n` followed by the diagnostic. Both
environment entries are consumed before executing startup, so nested programs
do not inherit another request to boot. This is a host completion contract,
not new Seed syntax or another interpreter.

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
script. Scripts contain successive expressions; open groups and leading-pipe
lines continue the current expression. UTF-8 BOMs and Unicode paths are supported. Source
errors report the file and line, stop execution and return nonzero. Seed supports
`#` line comments and `#[bracket comments]`, including nested bracket comments.
Foreground interactive tools use the interactive shell.

## Seed and shell semantics

These are different values and operations:

| Form | Meaning |
| --- | --- |
| `foo` | Resolve a symbol/binding or a host program. |
| `-foo` | Name: semantic identity of foo. |
| `"foo"` | Literal text. |
| `()` | Empty value. |
| `a b`, `a(b)`, `b \| a` | The same application, in three orientations. |
| `a () b`, `b : a` | The separate abstraction/capture mirror. |
| `greeting := "hello"` | Bind the RHS form to greeting. |
| `"hello" = greeting` | The same binding, with the expression first. |

Names use `-name`; attached `:name` is no longer Name syntax. Colon constructs
abstractions regardless of adjacent spaces. Names are not converted to literals
in the evaluator. Native `:=` and inverse
`=` retain the expression form; they do not memoize a foreign allocation. The
binding statement reduces its returned expression, and demanding a symbol
reduces its current binding. The existing Program ABI carries an implementation
and opaque host context without adding evaluator value kinds.

**Syntax migration:** old `name = expression` scripts must use `name := expression`
or `expression = name`; old `:name` identities become `-name`. There is no heuristic compatibility mode. TOML `=` is
unchanged. See [Seed operator contract](SEED_OPERATORS.md) for grouping and identities.

```text
greeting := "hello"
greeting
if -true "yes" "no"
if -false "yes" "no"
item := "previous"
for -item -("alpha" "beta") item
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
{ python -c "print('z'); print('a')" | sort }
```

`consumer producer`, `consumer(producer)` and `producer | consumer` are one
Seed application: the producer's completed answer becomes a native consumer's
stdin. `{ producer | consumer }` is the POSIX pipeline context: native stages
run concurrently over OS pipes and stream as they produce, and
`consumer {pipeline}` feeds a consumer live. Interactive foreground programs use
ConPTY with raw input forwarding, resize and interrupt support. `exit` and `quit`
end the shell.

## Optional capabilities and background work

`provide` transports inert typed values to a linked, process, or optional native
provider. Provider code lives outside Seed's evaluator. `capability register`
selects an executable; `capability register-library` selects a C ABI library.
The shell does not contain a renderer client or a renderer protocol.

`data bind -name EXPRESSION` evaluates once and retains the result. Ordinary
`name := expression` deliberately retains the executable expression instead.
`data list` creates an evaluated collection. `for` and `apply` consume structured
provider lists directly, preserving element identity and numeric representation.

`job` is ordinary shipped Seed vocabulary over the generic jobs provider. It
starts native programs asynchronously and exposes IDs, progress, cancellation,
exit status, results, timeouts, revision slots and opt-in reproducible caching.
See [typed capabilities and jobs](RUNTIME_CAPABILITIES.md) for the public contracts.
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
the canonical root. Opening sets the working directory and `HIRARI_WORLD` for
child processes. Validation/entry failures restore the previous world selection,
working directory and environment. Already performed entry side effects remain
owned by that entry. The entry is ordinary Seed source, with the same script failure rules.

## Embedding API

`Host` receives stdout/stderr, interruption and terminal capability callbacks.
`Shell::execute(source)` returns a Seed value, or null with a host diagnostic on
error. `Shell::execute_file(path)` runs expressions in the same evaluator and
returns success/failure. `Shell::status()` exposes the last execution status;
`seed()` and `worlds()` permit host inspection.

WorldHost handles descriptor validation, selected roots and launch rollback.

## Current regression suite

CTest registers eight production targets:

| Test | Coverage |
| --- | --- |
| `seed_semantics` | Reader/printer round trips; application and capture mirrors; binding orientation, lazy effects and stable cells; provider demand; comments and continuation. |
| `hirari_capabilities` | Linked PCRE2, structured predicate snapshots, inert provider identities, registration and non-executing help. |
| `hirari_shell_completion` | Foreground shell lifecycle, interruption and completion behavior. |
| `hirari_integration` | Neutral world/script validation and rollback; child environment; Unicode argv/files; control forms; live pipes; exit/quit. |
| `hirari_host_capabilities` | Single evaluation, typed collection iteration, exact numbers, foreign reference ownership, optional library registration, jobs, progress, cancellation, timeouts, staleness and caching. |
| `hirari_eval_name` | CLI preserves a Name. |
| `hirari_eval_failure` | Missing program returns nonzero. |
| `hirari_cli_usage` | Invalid CLI options return nonzero. |

The native integration tests compile their own neutral process helper and provider. Their worlds are temporary neutral fixtures, and PATH is restricted to
test binaries plus Windows system programs. They do not rely on developer
Python/Git/Bash installations. The fixture uses binary stdout for exact byte
assertions; production process output remains unchanged. These tests do not
claim actual Unity-player, bundled-tool or clean-machine installer acceptance;
those are Skunkworks package tests.

Re-run CTest for each release. `SEED_DEFERRED_EXPERIMENT=ON` adds isolated
experimental executables and three differential tests; it stays off in the
production shell. PCRE2 is statically linked from a version/hash lock, and its
source archive and notices are retained by release packaging. See
[capabilities](CAPABILITIES.md) for regex, provider dispatch and query boundaries.

Installed discovery, writable roots and the executable icon: [installation](INSTALLATION.md).

Optional machine-checked abstraction/substitution proofs and shared C++
conformance checks: [Seed verification](formal/README.md).
