# Seed essentials (read before writing any Seed)

These essentials are always loaded. The full, verified snapshot is `SEED_SPEC.md`, which sits next to this file in the hirari repository. The hirari source and tests are authoritative; when this file disagrees with them, trust the implementation and refresh the docs.

This describes hirari branch `seed-blocks-operators`. On `main`, redirection is unbraced and there are no operators, `=>`, line blocks or `[ ]`.

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

**Composition and worlds:**
- `source FILE` keeps the file's definitions.
- `run FILE ARGS` isolates the file and returns its last value. Inside it, `arg 0`, `argc` and `args` read the arguments.
- `script-directory` and `path` build paths.
- `world open "ROOT"` reads a strict `world.toml` (`[world] name, version = 1`, `[entry] program`) and runs that world-owned Seed entry.

**Look before you build:**
- `help` lists the built-ins, and `help -NAME` (operators: `help -(=>)`), `inspect -NAME` and `capability list` describe them. `EXPR =>` shows the next step.
- `Commands/*.seed` docstrings, the tests in hirari's `tests/`, and `SEED_SPEC.md` are the reference.
- Never invent operators or change Seed semantics. Changing Name identity, binding laziness, capture or substitution requires the owner's explicit approval and conformance tests.
