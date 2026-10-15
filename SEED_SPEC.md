# Seed language specification: implementation snapshot

This is a snapshot of how Seed and hirari actually behave, derived from the implementation. It is Claude Code's working understanding and is not a competing authority. **The source and the passing tests are authoritative.** When this file disagrees with them, the implementation wins: refresh this file (see the last section) and never "fix" the code to match the docs.

| Snapshot | |
|---|---|
| Source | hirari branch `seed-blocks-operators` (working tree on top of `6b9baa7`), hirari 0.5 |
| Differs from `main` | operators and arithmetic, `=>`, line blocks, `[ ]` lists, `{ }` redirection, strict unknown programs |
| Verified binary | `build/Release/hirari.exe` from that tree |
| Tests | ctest 14/14 including the Lean conformance gate, 2026-10-15 |
| Known flaky test | `hirari_shell_completion` vocab-history assertion (see Open issues) |
| Method | Every example marked `→` was run against that binary |
| Built-in list | from `hirari --eval help` |
| Program descriptions | from `help -NAME` (the registry's own text) |

Related reference documents in this repository:

- [SEED_OPERATORS.md](SEED_OPERATORS.md): the operator contract
- [README.md](README.md): the shell
- [RUNTIME_CAPABILITIES.md](RUNTIME_CAPABILITIES.md): providers and jobs
- [CAPABILITIES.md](CAPABILITIES.md): regex and dispatch
- [formal/README.md](formal/README.md): what is formally proved

## 1. Values, atoms and brackets

| Source | Kind (`data kind`) | Meaning |
|---|---|---|
| `foo` | `-symbol` | Resolved at evaluation: a binding, a built-in, a Seed command file or a native program. A bare unbound symbol is a value. **Applying** an unknown program fails, wherever it is nested (`program not found`). |
| `-foo` | `-name` | A Name: an inert semantic identity. It is never resolved and never turned into text by the evaluator. |
| `"foo"` | `-literal` | Text. `"""…"""` is multi-line text. `\n \r \t \\ \"` are decoded. |
| `3`, `-3` | `-integer` | Signed int64. A number never resolves as vocabulary. |
| `3.5`, `1e3` | `-float` | Finite binary64. |
| `()` | `-unit` | The empty value. It is also falsy. |
| `[1 2 3]`, `[]` | `-list` | A list: each element is evaluated once, exactly like `data list`. |
| `_` | | Fresh anonymous position. It is never a variable or a "previous result". |
| `-true`, `-false` | `-name` | Booleans are Names. |

| Brackets | Responsibility |
|---|---|
| `( )` | Application and grouping. A `(` that ends its line opens a **line block** (§6). |
| `[ ]` | List literal. |
| `{ }` | Live POSIX streams: `\|` pipe, `<` stdin from a file, `>`/`>>` stdout to a file (§8). |
| `#[ ]` | Nestable multi-line comment. `#` alone runs to the end of the line. |

- **Reified forms:** `-(a b)` is a reified (inert) product. `-()` is Unit as a Name, and `-(>)` names a glyph.
- **Operator Names:** `-pipe -bind -lt -le -gt -ge -eq -ne -and -or` name the Programs `| = < <= > >= == != && ||`. In contrast, `-(lt)` names the plain symbol `lt`.
- **`:foo` is not a Name.** The old attached `:name` syntax was removed. A colon always builds an abstraction (§3).
- **Shebang:** every script starts with `#!/usr/bin/env hirari`.

## 2. Application: one relation, three spellings

```seed
id := x : x
data print (id 5)      # → 5
data print (5 | id)    # → 5
data print (id(5))     # → 5
```

`a b == a(b) == b | a`. Outside braces, `|` is **reverse application**, not an OS pipe.

- **Grouping:** grouping one element is transparent (`() == (())`).
- **Products:** a product is an ordered list of terms. The head is applied to the rest.
- **Inert heads:** a product whose head is a value (a number, text, a Name), such as `5 5`, stays as data.

## 3. Abstraction (`:`) and substitution

`BODY : BINDER` is the same as `BINDER() BODY`. **The body is on the left and the binder is on the right.** This is the most common mistake.

```seed
twice := (data list x x) : x
data print (twice 4)               # → -((-list)(-((4)(4))))
data print ((x : x) 7)             # → 7
g := "x" : x                       # the body is the literal "x"
data print (g 1)                   # → "x"
```

Writing `x : (data list x x)` is the reverse: binder `(data list x x)`, body `x`.

Substitution:
- **Raw insertion:** application substitutes the raw argument structure.
- **Shadowing:** substitution stops at a binder with the same identity.
- **Intentional capture:** an inner binder may capture an incoming symbol. There is no alpha-renaming, gensyms or hygiene.

  ```seed
  (x() (y() x)) y        # → y() y   (formally proved, not renamed)
  ```
- **Opaque values:** Names, literals and cells are opaque to substitution.
- **Effects:** an unused argument stays unevaluated, and a duplicated effectful argument may run twice.
- **`_`:** `_()` discards an argument.
- **A registered Program is never a binder.** `if () "a" "b"` calls `if` with a Unit condition (→ `"b"`). A user name followed by `()` is still an abstraction.

**Formally proved** (Lean, `formal/`): determinism of one weak-head step, the identity, constant-empty and anonymous laws, shadowing, intentional capture, raw insertion, the opacity of Names and payloads, and eager ≡ deferred substitution. **Not proved:** dispatch, binding lookup, operators, providers, processes and serialization. Those are covered by the C++ test suites only.

## 4. Binding, cells, demand and `=>`

| Form | Behavior (observed) |
|---|---|
| `name := EXPR` | Binds the **lazy form**. The binding statement evaluates `EXPR` once immediately, and **every later reference re-evaluates it**, so effects repeat. |
| `EXPR = name` | The same binding, written expression-first. `=` is not equality (`==` is). |
| `data bind -name EXPR` | Evaluates `EXPR` **once** and retains the result. Use this for values, handles and process answers. |
| `@name` | The stable binding cell. It does not read the head. |
| `*name`, `* name` | Prefix `*` reads and reduces the cell's current head. Infix `a * b` multiplies. |
| `form?` | Attached `?` means `demand form`. Demanding a callable binding runs it. |
| `? -NAME` | Standalone `?` is help. It never demands. |
| `EXPR =>` | **One reduction step**, answered reified (`-(…)`) without evaluating further. |

```seed
data print (((x() (x x)) 5) =>)      # → -((5)(5))
data print ((2 + 3) * 4 =>)          # → -((5)(*)(4))
data print ((2 + 3) * 4 => =>)       # → -(20)
twice := (x x) : x
data print (twice 4 => =>)           # unfold twice, then substitute → -((4)(4))
data print (attempt (data print "hi" =>))   # → error: "=> stops before an effect (running program data) …"
```

What one step does (the reducer's own order):
1. orient `|` (`b | a` → `a b`);
2. normalize a colon abstraction;
3. unfold a binding;
4. substitute an argument into an abstraction (β);
5. compute an arithmetic or comparison operator whose operands are values (left operand first).

Invoking anything else is an **effect**, and `=>` stops before it and names it: a builtin, a native process, a `{ }` pipeline, a binding, a line block, or a list. Demand with `?` to run it. A normal form steps to itself.

- **Named relations stay data until demanded:** `3 -lt 5` is a value, and `(3 -lt 5)?` → `-true`.
- **Definitions across files:** `source FILE` keeps the file's definitions in the caller's scope. `run FILE` restores the caller's definitions, working directory and world afterwards, and returns the program's last value.

## 5. Truth, control, comparison and arithmetic

| Value | Truth (`truth`, `if`, `while`, `&&`, `||`) |
|---|---|
| `()`, `-false`, `0`, `""` | false |
| `1`, `"x"`, `-true`, any list (even `[]`), the Name relation `3 -lt 5` | true |
| A process answer | its exit status: 0 is true |

`data print …` returns Unit, which is false: `(data print "a") && B` never runs `B`.

```seed
data print (if (1 < 2) "yes" "no")             # → "yes"; only the chosen branch evaluates
data print (if (where.exe "cmd") "found" "missing")   # process success → "found"
for -t ["oak" "pine"] (data print t)           # the previous binding of t is restored
data print (attempt (fail "bad"))              # → record status "error", error "bad"
```

**Comparison** (outside `{ }`):
- `< <= > >=` order two numbers or two strings; other kinds fail.
- `== !=` compare numbers by value, strings by bytes and Names by identity or structure. Different kinds are simply unequal: `1 == "1"` → `-false`.
- Operands may be compound: `1 + 2 < 4` → `-true`. A chain such as `1 < 2 < 3` is rejected; join comparisons with `&&` or `||`.
- Prefix form: `> 5 3` → `-true`.

**Arithmetic** is written with spaces:

```seed
data print (3 - 3) (3 -3)       # → 0 3 -3      (attached -3 is a number, so (3 -3) is a product)
data print (1 + 2 * 3) (10 - 4 - 3) (10 / 4) (8 / 2) (2.5 * 2) ("ab" + "cd") (- 5)
# → 7 3 2.5 4 5.0 "abcd" -5
```

- `* /` bind tighter than `+ -`, and all of them associate left.
- Integers stay exact, and overflow fails. Any real operand gives a real.
- Exact integer division stays an integer; otherwise the result is real.
- Dividing by zero fails.
- `+` also joins two strings, and `- x` negates.

**Precedence, loosest first:** binding (`:=` `=`), `=>`, `&&` / `||`, comparison, `|`, `+ -`, `* /`, then ordinary application. Group compound `if` conditions: `if (x < 3) "low" "high"`.

**Failure handling:**
- `attempt` never absorbs `exit` or interruption.
- `fail MSG` stops a script with `FILE:LINE` unless something observes it with `attempt`.
- `exit [STATUS]` ends the shell and stops shell-owned jobs.

## 6. Layout, files and line blocks

**Files:**
- A file is a sequence of top-level statements, and a newline ends a complete statement.
- A line starting with `|` continues the previous statement. `||` is a different token.
- A trailing `\` touching the newline continues a line.
- `\` has exactly two meanings: the escape character inside strings (`\n \t \\ \"`), and line continuation in base form. Anywhere else in base form it is an ordinary symbol character (`a\b` is one symbol), and it never escapes an operator.

**Groups:**
- A group that starts on the same line as its `(` is **one expression**; its newlines are whitespace:

  ```seed
  data bind -xs (data list
      1 2 3)                       # one expression
  ```
- A `(` that **ends its line** (after spaces, tabs or a `#` comment) opens a **line block**. Its lines are statements with the file rules, run in order in the enclosing scope, and the block answers the last value:

  ```seed
  if (count < 3) (
      data print "few"
      data bind -level -low
  ) (
      data print "many"
  )

  data bind -n 0
  while (n < 4) (
      data print n
      data bind -n (n + 1)
  )
  ```
- In hirari a line block behaves like a file: each line's result is shown, and a line whose process fails stops the block (`block line N (…) returned status S`).
- A leading string documents a block. A block with one statement is just that statement.
- `{ … }` and `[ … ]` never form blocks; their newlines are whitespace.

**Output in `--file` mode:** `hirari --file` presents each statement's value (Unit and docstrings are not shown), and each `:=` or `data bind` echoes its value. `hirari --eval SRC` evaluates one source.

## 7. Native programs and processes

Program resolution, in order:
1. bindings and built-ins;
2. ALL-CAPS names, which prefer Seed vocabulary (`WHERE`);
3. other names, which prefer a native executable on `PATH` (`where` reaches `where.exe`).

An explicit `.seed` suffix selects source, and an explicit `.exe` or `exec "path"` selects a native program.

```seed
data print (where.exe "cmd")
# → -stdout "C:\\Windows\\System32\\cmd.exe\r\n" -stderr "" -status "0"
data print (data get (exec "C:/Windows/System32/where.exe" "cmd") "stdout")
data print (data integer "42")      # → 42; native text is never auto-converted
```

- **Arguments:** native argv is built from argument values with no shell re-parsing.
  - A Name projects to its flag spelling (`-c` becomes `-c`).
  - `()` is an empty-string argument.
  - Quote ordinary words (`"camera"`) and arguments that spell an executable (`hirari_test_program "sort"`), because a bare `sort` names Windows' `sort.exe` as a producer.
  - Live CLR handles and records cannot become argv text.
  - `cmd /c "echo hi"` does not work, because of quoting. Call real executables.
- **The answer:** a process answer is `-stdout -stderr -status`.
  - `data get ANSWER "stdout"` / `"stderr"` are text.
  - `data get ANSWER "status"` is an **integer** (`0 < status` means failure), even though the printed answer shows it quoted.
- **Application with a native consumer:** in `consumer producer` (or `producer | consumer`), the producer runs to completion and its stdout becomes the consumer's stdin. No OS pipe is created.
- **Feeding a value as stdin:** demand it: `sort (data file "in.txt")?`, or use `{ sort < "in.txt" }`.
- **Other built-ins:** `path "C:/a" "b"` → `"C:\\a\\b"`, `script-directory`, `cd`, `pwd`.
- **Environment:** `data env "NAME"` reads a variable (Unit when unset); `data env "NAME" VALUE` sets it, and `()` unsets it.
- **Bare built-ins are values.** Inside an argument list a bare built-in is not called: write `(pwd)`.

## 8. `{ }`: live POSIX streams

```seed
{ where.exe "cmd" "where" | sort }        # top level: streams to the terminal
{ sort < "in.txt" > "out.txt" }           # stdin from a file, stdout to a file
{ where.exe "cmd" >> "log.txt" }          # append
{ [1 -two "three"] > "values.txt" }       # a Seed value: text as bytes, data as an envelope
data bind -r { sort < "in.txt" }          # nested: answers -stdout -stderr -status -stages
```

Inside braces, `|` is a real OS pipe, `<` feeds the first stage's stdin from a file, and `>`/`>>` write the last stage's stdout to a file.
- **Stages:** every stage is a native process (a `PATH` executable or `exec "path" …`). Seed callables and `.seed` scripts are rejected.
- **Redirection placement:** `<` belongs to the first stage, and `>`/`>>` end the context. Each appears at most once.
- **Values:** a single non-process producer is written as text bytes or a `hirari.values/1` envelope, and `data read "file"` restores it.
- **Streaming:** stages run concurrently with backpressure. `consumer {pipeline}` adds a live final stage.
- **The answer:** nested in an expression, the answer adds `-stages`. The status is the last stage's, or else the first non-zero.
- **Stopping:** after the last stage exits, any stage still running after 2 s is ended (status 141). Ctrl+C or cancelling the job ends all stages.
- **Outside braces:** `<` and `>` compare values, and `>>` is an error. Redirection is never written outside `{ }`.

## 9. Data

```seed
data bind -r (data record -a 1 -b "two")
data print (data get r "b")                  # → "two"
data print (data at [10 20 30] 1)            # → 20   (zero-based)
data print (data size [1 2 3])               # → 3    (lists and records only)
data print (data json (data record -a [1 2]))   # → "{\"a\":[1,2]}"
data print (data parse-json "{\"a\": [1, 2]}")  # → a record; JSON null becomes Unit
data print (apply -(data list) -(1 2 3))     # apply a reified program to a reified argument product
```

Other `data` operations: `form` (a structural snapshot for providers), `text`, `real`, `file` (a file's bytes as text), `read` and `kind`. `for` and `apply` consume lists directly.

## 10. Composition: scripts, modules and arguments

```seed
source (path (script-directory) "lib.seed")      # the library's definitions stay visible
data print (run (path (script-directory) "child.seed") "a1" "a2")   # isolated; returns its last value
```

- **Arguments:** inside a program, `arg 0` (zero-based), `argc` and `args` read its arguments. `apply -(exec "tool") (args)` forwards them.
- **Command files:** a `.seed` file on `HIRARI_PATH` is a command, and its leading `"""docstring"""` is its help. For example, `job` is `Commands/job.seed`.
- **Persistent vocabulary:** `vocab save | list | history | restore | forget | use`. `HIRARI_STATE` selects the store, and `-` disables it.

## 11. Introspection: look before you build

| Use | To |
|---|---|
| `help` | List every built-in. |
| `help -NAME`, `? -NAME`, `NAME help` | Read a description, its arguments, origin and revision. The subject is never executed. Operators need grouping: `help -(=>)`. |
| `inspect -NAME` | See retained structure, bindings and provider identity. |
| `capability list` | List linked and registered providers (`jobs`, `regex`, …). |
| `EXPR =>` | Step an expression to see which binding or abstraction applies next. |
| `unity help REF [MEMBER]` (Skunkworks) | Read live CLR signatures. |

The built-ins are:

```
&& : := = ? @ _ apply arg argc args attempt capability cd data demand exec exit fail for help if inspect path provide pwd quit run script-directory sleep source truth vocab while world || > < <= >= == != >> + - / => [] | * {}
```

The shipped commands are `job` and `regex`. Skunkworks adds `System/Commands/*.seed`, which includes `unity`.

## 12. Providers, jobs and regex

- **`provide`:** `provide -CAPABILITY ARGS…` or `provide "EXECUTABLE" ARGS…` uses the typed `hirari.values/1` contract. Results are inert data.
- **Registering providers:** `capability register -id "tool.exe"` for a process, `capability register-library -id "lib.dll"` for a C ABI library.
- **Jobs:**

  ```seed
  data bind -j (job start "postgres.exe" ["-D" "C:/db" "-p" "55439"])
  job status (data get j "id")
  job wait (data get j "id") 10000    # never stops the job
  job cancel (data get j "id")
  ```

  - Jobs belong to this shell. They run in Windows Job Objects with no breakaway, so they stop when the shell exits.
  - A job's executable resolves through `PATH`.
  - Jobs are the way to run a session-scoped server.
- **`exec` versus jobs:** `exec` waits for the program and kills its process tree when it returns, so a daemon started by `pg_ctl` dies. Serve it as a `job` instead.
- **Regex:** `regex "parse" "(?<k>[a-z]+) (?<n>[0-9]+)" "oak 3"` uses linked PCRE2.

## 13. Worlds and embedding

- **`world open "ROOT"`:** reads a strict `ROOT/world.toml` and runs the entry program:

  ```toml
  [world]
  name = "Example"
  version = 1

  [entry]
  program = "enter.seed"
  ```

  - It sets the working directory to `ROOT` and `HIRARI_WORLD` to the root, and prints `open NAME ROOT`.
  - A failure restores the previous world, working directory and environment.
  - The entry is ordinary Seed and remains responsible for its own side effects.
- **`world status`** reports the current world.
- **Startup:** `HIRARI_STARTUP` (a file) runs once before the interactive prompt. `HIRARI_STARTUP_STATUS` (a file) receives `ready\n` or `failed\n<diagnostic>`. Both variables are consumed so that children do not inherit them.
- **`--file` and `--eval`** skip the startup hook.

## 14. Skunkworks (Unity) vocabulary

This vocabulary needs a running Skunkworks player; it is not exercised by hirari's tests.

```seed
data bind -type (unity type "UnityEngine.GameObject")
data bind -obj (unity new type "Proof")
unity set obj "name" "Renamed"
unity call (unity find "window") "SetIconFile" "C:/icon.ico"
unity wait env "IsSurfaceReady" 90000
```

- **The provider:** `unity` is `System/Commands/unity.seed`, which lazily registers `skunkworks_client.dll` and forwards arguments to `provide -unity`. hirari itself has no Unity code.
- **Operations:**
  - `connect`, `status`, `disconnect`
  - `find`, `type`, `component`, `get`, `set`, `call`, `new`
  - `call-exact`, `new-exact`, `call-generic`
  - `array`, `record`, `buffer`, `bytes`, `release`, `batch`
  - `wait`, `timeout`, `help`
  - `i32`, `i64`, `f32`, `f64`
- **Reflection roots** (`unity find`): `window`, `terminal`, `reflection` (`RegisterRoot`), `dispatch`, `values` and `boundary`.
- **References:** session-owned handles. Retain them with `data bind`, and build argument lists containing references with `data list` or `[ … ]`.
- **Request deadline:** 5 s by default (`unity timeout MS`). `SKUNK_UNITY_PIPE` selects the player.

## 15. Rules for agents

1. **Seed is the composition language and hirari is the shell.** Compose with Seed programs and hirari built-ins. Never wrap hirari in Python, PowerShell or Node orchestration to sequence a world or a session.
2. **Python is for specialised computation** (meshes, numerics), called as a native program. It is never a replacement shell.
3. **`a b`, `a(b)` and `b | a` are application.** Only `{ a | b }` is a live OS pipe, and redirection lives only in `{ }`.
4. **Don't invent operators,** and don't assume Bash, Python or JS semantics. Use only the operators listed here.
5. **Check before you build.** Before implementing a "missing" capability, run `help`, `help -NAME`, `inspect -NAME` and `capability list`, read `Commands/*.seed`, and look at the tests.
6. **Preserve the semantics.** Keep Name identity, binding laziness, capture and substitution exactly as they are. Never change Seed semantics silently; any change needs the owner's explicit approval and conformance tests.
7. **Prefer runtime introspection and docstrings** over hard-coded knowledge tables.

## Open issues

- **`main` versus this branch:** `main` still has unbraced `>`/`>>` redirection, no arithmetic or `=>`, no line blocks or `[ ]`, and silently inert unknown programs. Code written for this branch needs the branch's hirari.
- **Reified forms print structurally:** `=>` results print as `-((5)(5))`, not `-(5 5)`.
- **`hirari_shell_completion` is intermittent:** "restore/reload duplicated durable history commits" (`tests/shell_completion_tests.cpp:211`).
- **Skunkworks always sets `SKUNK_DATABASE` and `SKUNK_TOOLCHAIN` to its install root,** overriding terminal-profile values. The fix is on the Skunkworks branch `relocatable-toolchain-database`.
- **NightSky entry programs** use line blocks. They now parse as intended, but have not yet been run end to end on this branch inside the player.

## Refreshing this snapshot

Refresh whenever `seed/`, `hirari/`, `library/` or `tests/` change. Do not use generators or Python scripts for this.

1. **See what changed:** `git -C <hirari> log --oneline 6b9baa7..HEAD -- seed hirari library tests` and `git status`. If nothing touched those paths, the snapshot still holds.
2. **Compare the built-ins:** build, run `hirari --eval help`, and compare against the list in §11. Run `help -NAME` for anything added or changed.
3. **Run the tests:** `ctest --test-dir build -C Release`. A failing or changed test describes behavior that this file must follow.
4. **Re-check the examples:** re-run the `→` examples (`hirari --file` on a scratch copy). Update every statement that changed, record the new revision in the table at the top, and move fixed items out of Open issues.
5. **Treat contradictions as findings.** When this file and the implementation disagree, report it. Never "correct" the code to match the docs.
