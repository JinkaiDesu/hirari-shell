# Sibling capabilities

Seed supplies application, abstraction, Names and physical values. Providers
own their algebras. SQL words are ordinary command files, not lexer keywords;
the linked PCRE2 library owns regex syntax, not the Seed reducer.

```seed
#!/usr/bin/env hirari
regex "parse" "(?<label>[A-Za-z]+) (?<amount>[0-9]+)" "oak 3\npine 4"
```

The native build copies `library/regex.seed` into a sibling `Commands` folder.
Standalone Hirari therefore has regex without a database, Unity, another shell
or a regex subprocess. PCRE2 10.49 is statically linked into a separate host
module, pinned by source URL and SHA-256 in `cmake/regex-library.lock.json`.
`FETCHCONTENT_SOURCE_DIR_PCRE2` selects a verified local source for offline builds.
The upstream license is preserved in `Licenses/PCRE2-LICENSE.md`.

## Common dispatch contract

```seed
#!/usr/bin/env hirari
capability list
help -regex
provide -regex "match" "^\\w+$" "日本"
capability register -example "C:/tools/provider.exe" "Example typed provider"
provide -example "operation" 3 -identity
```

Linked modules register a callback through `Values::link`. External programs
register an executable path through `capability register`. Both receive the
same wire-encoded arguments and return inert wire-encoded values. Existing
`provide "EXECUTABLE" ...` remains valid. External programs use `--values` and
the `hirari.values/1` envelope; linked modules bypass process startup. A duplicate
external registration for the same path is idempotent. A different path or a
linked identity cannot be silently overwritten.

Names and Symbols have explicit tagged transport representations; accepting
them does not turn them into text. Records, lists, integers, reals, strings,
Booleans and Unit remain typed. Live Unity handles and binding cells stay local.
Providers must explicitly decide which structural arguments they accept.

`data form VALUE` snapshots structure for a provider. An abstraction retains
its binder and inert body; free retained bindings are recursively snapshotted,
without executing captured program forms. This is a generic structural bridge,
not a SQL evaluator. The argument is resolved normally, so use an abstraction
when the body must remain unevaluated. Provider interpretation can reject
unsupported operations without executing them.

Help reads capability descriptions and command source metadata without calling
the provider. Retained bindings and builtins take priority. All-capital command
identities prefer Seed vocabulary before implicit executable lookup; other names
prefer native executables, falling back to vocabulary. Explicit `.seed` selects
source; `.exe` spelling or `exec` selects native programs. Help and pipe planning
follow the same rule. Thus `WHERE` selects vocabulary while `where` still reaches
Windows' native `where.exe`.

## Regex dialects remain separate

`regex` / `provide -regex` selects PCRE2. `postgres-regex` selects PostgreSQL ARE
when the external vocabulary and database are available. CLR reflection can
still reach `System.Text.RegularExpressions.Regex` inside Unity. No universal
regex dialect is implied.

PCRE2 operations: `info`, `pattern`, `match`, `capture`, `parse`, `replace`.
`parse` infers `(?<name>...)` captures or takes an explicit column list;
`replace` uses PCRE2 `$1` / `${name}` replacement syntax. `pattern` creates an
inert engine-tagged pattern value. An engine-tagged pattern cannot be routed
silently into another regex engine.

Flags: `i` ignore case, `m` multiline anchors (`n` is an explicit alias), `s`
dot-all, `x` extended, `g` global, `q` literal pattern. With no flags, capture,
parse and replace process all matches. With flags, include `g` for all matches.
UTF-8 and Unicode properties are enabled. `q` cannot combine with `m/n/s/x`.
Unmatched groups remain Unit. Regex answers are materialized, bounded to 100,000
rows / 16 MiB captured row payload; external captured output also has the existing
16 MiB limit. Match/depth budgets limit matching work; these are not timeouts.

The PostgreSQL vocabulary and its supported predicate lowering live in the
Skunkworks repository. Neither SQL planning nor regex semantics enters `seed.cpp`.
