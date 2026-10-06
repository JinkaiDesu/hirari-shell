# Seed operator contract

These orientation laws are executable in standalone Seed:

```text
a b       == b | a       == a(b)
a () b    == b : a
a := b    == b = a
```

The first row is ordinary application/program product. Chained flow is
`producer | middle | consumer`, forming `consumer (middle producer)`. Seed
constructs that product without executing the producer during reorientation.
Hirari uses the same transformation and specializes OS program composition
as concurrent, live process pipes. Native Programs decide how to demand arguments.

The second row is capture/reorientation. The Unit remains in the product.
`a : b` is therefore different from `a :b`, which applies `a` to Name(b).
An OS process adapter does not implement capture products: it diagnoses a Unit
in the capture position instead of silently treating it as ordinary application.
Elsewhere Unit passed as an OS argument becomes empty text at the argv boundary.

The third row binds an identity to a retained expression. Both orientations
update the same stable cell, increment its revision, and return the expression
for normal reduction. Evaluating the binding statement can execute that expression;
subsequent demands execute it again. Neither spelling captures a foreign allocation
once. Use `unity bind :name expression` for that host-specific operation.

```text
greeting := "hello"
"hello" = greeting
@ greeting               # the stable cell
* greeting               # demand its current head
```

This deliberately changes old name-left `=` syntax. Migrate it to `:=`.
`=` is the expression-first mirror of binding here, not Boolean equality or a
universal assertion/unification engine. Names, Symbols and host-supplied Cells
are valid destinations; arbitrary expressions are not destinations.

At a product level, binding is processed before flow, and flow before detached
colon, before ordinary native/foreign program invocation. Group nested operations
explicitly. For example `result := (producer | consumer)` or
`producer | consumer = result`. Multiple bindings in one ungrouped product are
an error. Operator Symbols used as data should be named/reified, not left bare.

`:=` is recognized before attached `:` and can adjoin its operands (`x:="value"`).
Use spaces around other infix operators. `:name` remains a Name, not text.
Operator identities affected by the walrus lexer print as `:(=)` and `:(:=)`.

`? expression` demands the expression. A query provider can use the existing
Program ABI to return matching identities, including an empty or plural answer.
Seed supplies no automatic inverse solver, SQL backend, or free-variable search.
Postfix `expression ?` has no new demand meaning. Keep `?` separate from its
operand: `?name` remains a different Symbol.

Comments are lexical, outside quoted text:

```text
#[A multiline explanation
  #[with a nested comment]
]
greeting := "# this remains text" # comment to newline
```

`#[` opens a bracket comment and `]` closes it; quotes and parentheses inside
comments have no syntactic meaning. Unquoted `#` starts a comment, including
inside an otherwise bare word: quote OS arguments containing it. The reader,
interactive continuation prompt and script runner share these rules. Incomplete
groups, strings and bracket comments wait for continuation; EOF in a script is
an error reported with the expression's starting file/line.

This pass adds no `!`, `!=`, general class grammar or new `@`/`*` overloads.
