# Seed proof and conformance checks

This package formalizes the existing abstraction/substitution kernel. It does
not change Seed's pointer representation, evaluator, syntax, or public ABI.
Lean is an optional developer dependency; it is never shipped as a shell
runtime dependency.

## What is proved

The Lean kernel checks these statements for arbitrary finite forms:

- One weak-head abstraction step has at most one result.
- Identity, constant-empty, and anonymous abstractions follow their current laws.
- Substitution stops at a binder with the same identity.
- Other binders may intentionally capture an incoming identity.
- Names and opaque payloads are not traversed by substitution.
- Replacement terms are inserted raw, not recursively rewritten by that same
  substitution.
- A term without a free occurrence of the target remains unchanged.
- The executable step function used by the corpus generator implements exactly
  the formal step relation.
- Chronological deferred substitution, with shadowed pending frames removed,
  produces the same body as eager substitution through a curry spine.

For example, `(x() (y() x)) y` produces `y() y`. It does not silently
rename the inner binder. The deferred representation is an ordered list of
pending substitutions, not a conventional hygienic lexical environment.

The fusion theorem covers the substitution/materialization stage. It does not
prove C++ dispatch guards, step accounting, documentation stripping, binding
lookup, or foreign execution. Equality is preserved under any subsequent
observer; the observer itself is not specified or verified by that corollary.

## Connection to the implementation

`Main.lean` generates a deterministic TSV corpus with independently computed
expected forms. The native runner reads that same corpus with Seed's real
reader, then tests both literal substitution and reduction. It compares
canonical observations without reducing the expected answer in C++.

The corpus covers repeated binders, intentional capture, retained Names,
anonymous binders, empty bodies, nested applications, and opaque literal
payloads. Curry cases use stable arguments to exercise the optimized path;
string/docstring dispatch is outside this kernel. The runner also checks that
retained source products and an unrelated binding are unchanged, and that an
opaque host Name retains its pointer identity.

Flat products and explicitly nested left applications have the same
application observation in these tests. Pointer addresses, allocation counts,
and exact parenthesis printing are not that observation. Every row runs in a
fresh runtime with no host providers.

The existing differential suite separately covers effects, live bindings,
mutable templates, metadata inspection, optimization guards, and budget edges.
These executable checks are evidence of conformance, not a proof of the C++
implementation, memory safety, native programs, concurrency, or the full shell.

## Run

Install the official Lean toolchain pinned by `lean-toolchain` (4.19.0).
The tested Windows release URL and hash are recorded in `toolchain.lock.json`.
No Mathlib or other external Lean packages are required.

With the pinned toolchain on PATH, proof checking alone is:

```text
cd formal
lake build
lake env lean -DwarningAsError=true Audit.lean
```

To include both native reducers in the normal Windows CTest suite:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON -DSEED_DEFERRED_EXPERIMENT=ON `
  -DSEED_LEAN_PROOFS=ON -DSEED_LEAN_LAKE=C:/path/to/lean/bin/lake.exe
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

`formal/verify.ps1` builds the proofs, audits theorem dependencies, generates
the corpus, and runs the reference and optional experimental reducer. It also
requires two negative controls to fail for the expected reason: a theorem
claiming hygienic capture, and a corpus with an incorrect expected answer.

Evidence is written to `build/formal-evidence/`: compiler version, proof build,
axiom audit, shared corpus, reducer output, negative-control output, and
`result.json` with the corpus hash. A failed run does not publish a success
summary.

## Trust and limits

`Form` is a mathematical view of finite canonical products, not a proposed
runtime AST. String identities model symbols interned by one Seed runtime;
Names and literals are opaque to substitution. This model does not establish
the reader's numeric encoding, physical bounds, or arbitrary pointer graph
well-formedness.

Proofs contain no `sorry`, `admit`, custom axioms, or `native_decide`.
`Audit.lean` lists the dependencies of the public theorems. The runner permits
only Lean's standard logical dependencies (`propext`, `Quot.sound`,
`Classical.choice`) and rejects `sorryAx` or additional assumptions.

The proven result is deliberately narrow: eager and deferred substitution
agree for this kernel. Host-response determinism, error propagation, typed
provider values, serialization, global bindings, and operating-system effects
require their own specifications before they can be called formally verified.
