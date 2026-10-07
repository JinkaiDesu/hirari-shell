# Seed operator contract

```seed
a b       == b | a       == a(b)
a() body  == body : a
(a() body) value  -> body[a := value]
name := expression      == expression = name
```

Seed still uses pointer products and the Program ABI. Abstractions are ordinary
products containing binder, Unit, and body; there is no Lambda AST or evaluator.
Grouping is transparent for one element: `() == (())`. Consequently `a (|) b`
is `a | b`, yielding `b a`. A general relation-slot calculus and pipe involution
are **not** implemented; `a | | b` remains an empty-stage error.

## Atoms and identity

| Source | Reader value |
| --- | --- |
| `foo` | Symbol; eligible for resolution |
| `-foo` | Name(foo); inert semantic identity |
| `"foo"` | String literal |
| `3`, `-3` | Signed 64-bit integer literal |
| `3.14`, `1e3` | Finite binary64 real literal |
| `()` | Unit |
| `_` | Fresh anonymous position; no Cell |

Numbers carry machine values and retain their source spelling for lossless
printing. They never resolve as vocabulary. Overflow/nonfinite literals fail
explicitly. This physical substrate does not declare semantic types: `-int`,
`-number`, and `-meters` are ordinary Names. Providers define classification.
String ordering compares UTF-8 bytes; numeric ordering preserves int64 precision.

`"""multiline text"""` uses the same String payload as ordinary quotes. Newlines,
single/double quotes, and comment markers inside it are content. Escapes are
preserved; only the closing triple delimiter ends it. The continuation scanner
waits for that delimiter. A leading body string is documentation: help reads it
without execution. In an abstraction with remaining body structure it is skipped
on application; a lone string stays a constant. Script documentation is suppressed
when execution proceeds to the next form, and remains the result of a doc-only file.

Names use `-name`; old attached `:name` is removed. Reify a product with
`-(forms ...)`, Unit with `-()`, or a glyph with `-(>)`. Names remain values,
not strings. `-pipe`, `-bind`, `-lt`, `-le`, `-and`, `-or` name the existing
Programs `|`, `=`, `<`, `<=`, `&&`, `||`. `help -pipe` inspects that identity.
Explicit grouping preserves a raw identity: `-(lt)` names the Symbol `lt`,
while `-lt` names `<`. The printer retains this distinction.
At native argv boundaries a symbolic Name projects to its flag spelling:
`-c` becomes the bytes `-c`. Quote an ordinary non-flag argument such as `"camera"`.
Live CLR identities and structured records/lists require explicit projection.

## Abstraction and binding

All colon spellings agree: `a:b == a: b == a :b == a : b == b() a`.
The body can contain multiple terms; colon chains frame it from left to right:

```seed
identity := x : x
first := x : y : x
compose := f (g x) : x : g : f
(f x : x) 3                     # f 3
(a : b : a) b                   # b() b; intentional capture
3 : Int                         # Int() 3; no type assertion
"yes" : _                       # accepts and discards an argument
```

Application substitutes raw argument structure, respecting same-identity nested
shadowing. Incoming symbols may be captured by different nested binders.
No alpha-renaming, gensyms, hygiene, or lexical snapshot occurs. Names, literals,
and cells are opaque; retained bodies and global cells are not mutated.
An unused argument stays unevaluated; duplicating an effectful argument may
execute it twice. Divergence reaches the existing reduction budget.

`_` is never a variable, previous result, or implicit pipe hole. Each occurrence
is independent. `_()` discards an argument without substituting into other `_`
positions. Binding/referencing `_` is an error.

`r := 3` and `3 = -r` update the same stable Cell. `@r == @ r` returns that
cell; `*r == * r` reads/reduces its head. Binding retains a lazy expression,
then normally evaluates the statement result. Use `data bind -r expression`
or `unity bind -r expression` to evaluate and retain a result once.
`=` does not mean equality. `-r = 3` is not an accepted reversed binding.

An abstraction's body is inert, including operators inside it. Group an
abstraction before applying it or binding in reverse orientation:
`(x() x) = identity`. Pass an initial empty native argv with
`exec "program" () "argument"`; `program() body` denotes abstraction.

Abstraction recognition reads the linked product directly; substitution rebuilds
changed bodies without first constructing intermediate colon captures. These
are temporary structural views, not cached evaluations or a Lambda payload.
Repeated calls still observe rebinding and host edits, unused arguments remain
inert, and duplicated arguments may repeat effects. Extracting a flat body for
host inspection still creates an independently owned product spine.

With tests enabled, `seed_performance` measures reduction with fresh runtimes
and excludes parsing/printing. `seed_performance_profile` reports work counts
from a separate instrumented library; its counters are absent from Hirari.

## Demand, ordering, and control

Standalone `?` is Hirari help, never demand. Attached `form?` lowers to
`demand form`. Values retain their type and identity; a bare callable executes.
`-form?` reifies the demanded form, while `(-form)?` demands the Name itself.

A named relation such as `3 -lt 5` stays data. Explicit demand `(3 -lt 5)?`
invokes the named Program as `< 3 5`. The same generic mechanism supports
`(3 -is -int)?` when vocabulary supplies an `is` Program. Seed supplies no
universal classification rules, inverse solver, or SQL translator.

`<` is strict ordering; `<=` is ordering or equality of scalar values. Reverse
the operands for the inverse. Semantic equivalence/classification belongs to
vocabulary such as `is`. Group compound operands and `if` conditions:
`if (x < 3) ("low") ("high")`.

`A && B` evaluates B only after A succeeds; otherwise it returns A.
`A || B` returns successful A; otherwise it evaluates B. The selected value
and process status survive. Both have equal precedence and associate left,
as shell control lists do. Prefix Programs `&& A B`, `|| A B`, and
`apply -and -(A B)` use the same lazy behavior. `||` is one token; `| |` is two.

Binding owns its RHS, followed by short circuit, routing/comparison, flow, then
ordinary Program invocation. A colon-framed abstraction retains the whole
body before any effects. Group nested bindings and compound scalar operands.

## Output routing and program boundaries

Outside literals, spaces, tabs and physical newlines separate product terms;
indentation has no meaning. Use spaces for style; tabs remain legal. A trailing
`\` touching LF or CRLF continues source layout and skips following spaces/tabs:

```seed
a b \
    c
```

This reads as `a b c`, including when the backslash touches `b`. It never joins
tokens or creates attached syntax across lines. Other backslashes remain ordinary
symbol characters; a dangling continuation at EOF is incomplete source. Comments
do not continue source, and continuations inside literals are not stripped.

Hirari's prompt and script runner submit complete top-level forms at physical
line boundaries. Continuation suppresses that submission until the next line;
open groups/strings/comments also keep the form pending. Inside a submitted
product, ordinary newlines already behave like other whitespace.

Literal spaces and physical newlines/tabs are data. Ordinary and triple-quoted
strings both decode `\n`, `\r`, `\t`, `\\` and `\"`; triple quotes are already
supported. Source layout does not trim or dedent literal contents.

`producer > destination` writes; `producer >> destination` appends. A quoted
file path receives native stdout directly, without materializing large output
in Hirari. Native stderr and failure status remain in the process result.
String values write exact bytes. Other serializable values write a
`hirari.values/1` envelope, preserving Names, Unit, columns, and scalar types.
`data read "file"` restores one envelope. Appending structured values produces
a stream of envelopes, not an implicitly merged relation.

A callable destination consumes the structured producer normally; `>>` also
supplies `-append`, whose meaning the sink implements. No SQL, regex, or file
format ontology is baked into Seed.

Native process composition uses live pipes. Ordinary process arguments are
variadic products; `instantiate * @sky` does not automatically mean
`instantiate (* @sky)`. Group nested consumers explicitly. Typed provider
calls use `provide "executable" ...`; projection with `data json/text` is explicit.
`data integer ANSWER` / `data real ANSWER` explicitly decode native text into
numeric literals; a `wc` answer does not silently become a number. Integer
decoding rejects fractional values. Filesystem paths compose with `path`.

Every Hirari script starts with `#!/usr/bin/env hirari`. Forms execute in source
order. Physical EOF yields the last evaluated form (Unit for an empty file);
it does not require a `return`, `main`, or literal EOF token. `run` restores the
caller's definitions/cwd/world context; `source` retains definitions. An
unfinished group, string, or bracket comment at EOF reports its source location.

`#` comments run to newline. `#[ ... #[nested] ... ]` comments nest. Quoted text
is unaffected. The reader, continuation prompt, and script runner share these rules.
