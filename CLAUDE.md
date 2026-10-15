# hirari / Seed

This repository is hirari, the native Windows shell, together with Seed, its composition language. It is the canonical home of the Seed language context:

@SEED_ESSENTIALS.md
@SEED_SPEC.md

## Working here

- **Authority:** `seed/`, `hirari/`, `library/` and `tests/` define the language. `SEED_SPEC.md` is a snapshot of them. Follow its "Refreshing this snapshot" procedure whenever you change semantics, built-ins or tests, and record the new revision in its header.
- **Build and test:**

  ```
  cmake -S . -B build
  cmake --build build --config Release
  ctest --test-dir build -C Release --output-on-failure
  ```

  - The Lean gate (`formal/`) is optional and documented in `formal/README.md`. The test runner must not be Python.
  - Release folder: `cmake --install build --config Release --component hirari --prefix <dir>`.
- **Protected semantics:** never change the reader, reduction, Name identity, binding laziness, capture/substitution, `|` orientation or `{}` pipelines without the owner's explicit approval, and never without updating `seed_tests`, `integration_tests` and the Lean corpus where they apply.
- **Introspect first:** before adding a program, check `hirari --eval help`, `help -NAME`, `inspect -NAME` and `capability list`. New vocabulary should be a documented `library/*.seed` command or a provider, not a special case in the evaluator.
- **Help text:** help is derived from registry descriptions and docstrings. Do not hard-code tables in `shell_help.cpp`.
