# hirari / Seed

This repository is hirari, the native Windows shell, together with Seed, its composition language. It is the canonical home of the Seed language context. It says everything about itself in its own files: it shares only the published interfaces below with anything else, never files, scripts or instructions.

## Solo developer, fast iterate

One developer builds this, in the Motorcycle Architecture (coined by Terry Davis): embrace the footguns and keep moving. Refuse to obsess over safety purity; it doesn't exist. Nothing sits between an assignment and its result: no planner and no shared pipeline. The repository checks itself (Checks below), and an authorized objective runs through to a verified result (Authorization below).

- **No enterprise hedging:**
  - No backwards-compatibility layers, deprecation warnings or parallel "v2" systems.
  - No slow migration phases unless the owner asks for one.
- **Replace, then delete.** When something replaces an old system or script, delete the old one in the same change, and leave no dead code. Errors are good: breaking things is how old wrong assumptions are found. Software is a sacrificial pattern; some code must die.
- **Clean as you go.** Deleting files, unused configuration and legacy build setups in this repository is routine, because git keeps them. Pushed history and published releases follow Authorization.
- **Tests are behavioural only.** They never encode architectural conventions.
- **Provenance, process and product.** Only human artwork and docs and the full project sources need preserving: source artifacts, the procedural transformations applied to them, the final artifacts tied back to their source, and their composition. Heavy assets belong in data storage; git holds source and process.

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
- `library/*.seed` docstrings (installed as `Commands/`) and the tests in `tests/` are the reference.
- Never invent operators or change Seed semantics. Changing Name identity, binding laziness, capture or substitution requires the owner's explicit approval and conformance tests.

## Working in this repository

- **Authority:** `seed/`, `hirari/`, `library/` and `tests/` define the language. Whenever you change semantics, built-ins or tests, update the Seed essentials above to match.
- **Only two Markdown files:** this `CLAUDE.md` and the user-facing `README.md` (under 80 lines). The old long-form docs (`SEED_SPEC.md` and the others) live in history only, last at commit `e9d9269`; don't add them back.
- **Protected semantics:** never change the reader, reduction, Name identity, binding laziness, capture/substitution, `|` orientation or `{}` pipelines without the owner's explicit approval, and never without updating `seed_tests`, `integration_tests` and the Lean corpus where they apply.
- **Introspect first:** before adding a program, check `hirari --eval help`, `help -NAME`, `inspect -NAME` and `capability list`. New vocabulary should be a documented `library/*.seed` command or a provider, not a special case in the evaluator.
- **Help text:** help is derived from registry descriptions and docstrings. Do not hard-code tables in `shell_help.cpp`.

## Ownership and topics

One owner session takes each piece of work from assignment to verified result. Nobody else signs off.

- **A topic** is a branch `agent/<topic>` with its own worktree in `.worktrees/<topic>` (ignored by git), made with plain git:

  ```
  git fetch origin main
  git worktree add -b agent/<topic> .worktrees/<topic> origin/main
  ```

  Change files only in a topic's worktree.
- **The main checkout** is for integration only: it is fast-forwarded to `origin/main` after a merge, and never edited.
- **Overlapping topics** are the owner session's to sequence: merge dependencies first, then bring `origin/main` into the next one.
- **Decoupled:** no build elsewhere waits on hirari, and hirari waits on no build elsewhere. A pull request here never has a merge order with another repository's pull request.

## Checks

From the root of any clean checkout, on Windows with CMake 3.24+, a C++23 toolchain (Visual Studio Build Tools) and PowerShell 7:

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
pwsh -NoProfile -File tools/package.ps1 -Build build -Out out
```

- `package.ps1` writes `out/hirari-<version>-windows-x64.zip` with its `release.receipt.json`, then checks the archive on its own with `tests/release.tests.ps1`, which ends `HIRARI_RELEASE_OK tests=<n>`.
- `cmake --install build --config Release --component hirari --prefix <dir>` writes a release folder.
- The Lean gate (`formal/`, `lake`) is opt-in and not one of the checks. The test runner must not be Python.
- A check that can't run is a blocker: report what it needs, and never skip it or count it as passed.

## Interfaces

hirari publishes these and consumes none. A change to one bumps its version. Whoever reads them picks up a new version on their own; nothing here waits for them.

| Interface | Version | Contents |
|---|---|---|
| Release archive | `hirari-<version>-windows-x64.zip` | The `cmake --install` release folder, deterministic, with `release.receipt.json` (`schemaVersion` 1: `name`, `version`, and each file's `path`, `size` and `sha256`). |
| Download manifest | `emergence.manifest/1` | `hirari/<version>/manifest.json` in the public downloads bucket for each version, and `hirari/manifest.json`, the latest pointer: a byte-identical copy of the newest version's manifest. |

## Versioning

hirari has its own semver, `project(Hirari VERSION …)` in `CMakeLists.txt`. It answers to nothing outside this repository, and other projects' versions never constrain it.

- **hirari is pre-alpha.** It stays on `0.x`, every manifest's edition is `pre-alpha`, and no build is ever called stable. Leaving pre-alpha or going to `1.0` is the owner's explicit decision, never a pipeline step.
- **Every change to the shipped payload ships a new version.** `pipeline.ps1 stage` bumps the patch when `main`'s version is already live and the topic hasn't moved past it. A minor bump is a deliberate edit of `CMakeLists.txt` in the topic. `ship` refuses a pull request that changes the payload at a version that is already live.
- **The release record is git's:** each published version is the annotated tag `v<version>` on the commit its manifest names, with the manifest's and the archive's SHA-256 in the message. `tools/publish.ps1` makes and pushes it. A tag is never moved.

## Publishing

A version goes live when its archive and manifests are in the public R2 bucket `emergence-releases` (Cloudflare account `7e05861211be8c45fef9de909a25f833`), served at https://downloads.emergencetech.co. From a clean main checkout at a commit on `origin/main`, after the Checks:

```
pwsh -NoProfile -File tools/publish.ps1
```

- **It writes in this order,** so the pointer never names anything that isn't up yet:
  1. `hirari/<version>/hirari-<version>-windows-x64.zip`, then it checks the served bytes;
  2. `hirari/<version>/manifest.json` (`emergence.manifest/1`: edition `pre-alpha`, platform `windows-x64`, the commit, and the archive's `bytes` and `sha256`), cached as immutable;
  3. `hirari/manifest.json`, the same bytes, `no-cache`.
- **Then it checks and records the release:** https://emergencetech.co/api/latest/hirari must answer that version and commit, and it tags the commit `v<version>`.
- **A published version is never overwritten.** If the version's manifest is up and the pointer is its exact copy, it uploads nothing and prints `UNCHANGED`.
- **A run that stopped part-way is finished from what is live:**
  - a missing versioned manifest is copied from the pointer's exact bytes;
  - a pointer still on an older version is moved forward, never back;
  - an archive already up with the same bytes is reused, and one with other bytes is refused.
- **The token** is `CLOUDFLARE_API_TOKEN` from the process or user environment. It needs R2 write on the bucket and is never printed.
- **Nothing is signed.** Signing is the owner's decision (Authorization).

## Authorization

Bounded autonomous execution (owner decision, 2026-10-21). Once the owner authorizes an objective in this repository, the authorization covers that objective's whole normal lifecycle. Don't ask again for any step of it, including the steps that run on GitHub or Cloudflare.

- **Covered by the objective's authorization:**
  1. Make the topic's worktree and change only that worktree. Repair failing tests there.
  2. Commit and push `agent/<topic>`, and open or update its pull request.
  3. Gate and merge it (see Integration), and check `origin/main` afterwards.
  4. Bump the patch version, publish the new version's archive and manifests to the downloads bucket, verify them live, and tag the release.
  5. Report the pull request, the merged commit, what was published, what was checked and how long it took.
- **Still the owner's explicit approval, each time:**
  - Signing, and any change to licensing (GPL-2.0-only) or to the third-party licenses shipped.
  - Leaving pre-alpha, or going to `1.0`.
  - Changes to protected Seed semantics (see above).
  - Destructive actions on unique or irreplaceable data: overwriting or deleting a published version, deleting buckets or objects, and rewriting pushed history.
  - Security policy and credentials: secrets, tokens, repository access and visibility, branch protection, and Claude Code's permission settings.
  - Anything outside the authorized objective.
- **Blockers.** When a permission, authentication or approval blocker comes up, report it once with what it needs, and carry on with whatever doesn't depend on it. Don't retry equivalent operations.

## Integration

There is no CI. The repository is public, and GitHub enforces nothing on it: no branch protection, rulesets or required checks are set (checked 2026-10-21). This procedure is the gate. Merge a pull request only when all of these hold for its exact head commit:

1. **Checked:** the Checks passed on that commit in a clean checkout.
2. **Nothing failing:** every status or check GitHub reports on the head passed. A pending or failed one blocks the merge, and is never merged around.
3. **Dependencies first:** every pull request here that it builds on is merged. Pull requests in other repositories are never dependencies.
4. **Coordinated:** no other open pull request here changes the same files.
5. **Mergeable:** GitHub reports no conflict.

Then merge exactly that head: `gh pr merge <n> -R JinkaiDesu/hirari-shell --squash --match-head-commit <sha>`. After the merge:

1. Check `origin/main`.
2. Fast-forward the main checkout (`git merge --ff-only origin/main`). If it has local changes, leave them alone and report them once.
3. Retire the topic: `git worktree remove .worktrees/<topic>` and delete its local branch.

`tools/pipeline.ps1` runs this procedure, so work takes one command. Run it in the background and wait for it to exit, rather than polling:

```
pwsh -NoProfile -File tools/pipeline.ps1 start <topic>                    # .worktrees/<topic> on agent/<topic>
pwsh -NoProfile -File tools/pipeline.ps1 stage <topic> -Message <text>    # accumulate: check, commit, push, PR
pwsh -NoProfile -File tools/pipeline.ps1 ship <topic|number>... [-Message <text>] [-DryRun]
```

- **`stage`** is for small changes that collect on one pull request. Nothing is merged or published, and staging again adds more. It bumps the patch version first when the change touches the shipped payload (Versioning).
- **`ship`** takes the named topics or pull requests all the way to live now. With `-Message`, it stages the one topic named first.
- **For each pull request, in order,** `ship` checks the exact head in a throwaway `.worktrees/.ship-<n>`. It gates it (head unchanged, mergeable, nothing pending or failing, and a payload change at a version not yet live), then merges exactly that head.
- **After the last merge,** `ship`:
  - fast-forwards the main checkout and runs the checks there again;
  - runs `tools/publish.ps1` when the merged files touch the shipped payload (`hirari/`, `seed/`, `library/`, `cmake/`, `Licenses/`, `CMakeLists.txt`, `LICENSE`, `NOTICE`, `README.md`). A version already live is left as it is;
  - retires each topic's worktree and branches.
- **It never works around a failure.** It stops at the first blocked step and prints `BLOCKED at <stage>: <why>`, which is what to report. Otherwise the last line is `STAGED …`, `SHIPPED …` or `DRY RUN …`.
- **The main checkout must be clean.** `ship` refuses to update or publish over local changes.
