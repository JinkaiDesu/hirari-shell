# Optional startup composition

An interactive Hirari session evaluates the UTF-8 file named by `HIRARI_STARTUP`
after printing its banner and before its first prompt. The file uses the same
Seed evaluator as later input. Its bindings remain available in the session.
Errors print the usual file/line diagnostic and leave the shell available.
Without that variable, standalone Hirari starts normally. `--eval` and `--file`
do not implicitly execute startup composition.

Skunkworks supplies this variable only when its installation contains
`System/startup.seed`. The base installs no startup world. An application
distribution can put ordinary composition in that optional file, for example:

```seed
world open "Worlds/Example"
```

`world root` returns the selected world's canonical absolute root as text.
It fails when no world is selected. Use it to resolve resources before sending
their paths to another process; that process has its own working directory.
This command and startup entry add host capabilities, not evaluator syntax.
