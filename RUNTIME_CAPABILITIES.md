# Typed capabilities and background jobs

Seed is purish: bindings, reduction and deliberate effects coexist. Its foreign
effect boundary is the process/stream/provider contract. Windows currently
implements the process contract with native APIs and ConPTY. It does not require
Bash, another evaluator, a renderer or a database to execute a program.

## Values and identity

`provide -identity ...` calls a registered provider. Arguments and results use
`hirari.values/1`: unit, boolean, integer/float spelling, text, symbols, Names,
inert products, lists and records. Source received from providers is data.

```seed
#!/usr/bin/env hirari
data bind -rows (data list 1 2 3)
for -row rows (data print row)
apply -(data print) rows
```

`data bind` evaluates once. `:=` stores a lazy executable form and deliberately
does not memoize its effects. `data print` evaluates each argument once.
Numeric provider results fit the signed int64/double physical substrate; larger
unsigned integers retain their tagged representation rather than losing bits.
A provider may preserve physical width using `physical` i32/i64/f32/f64.

Opaque foreign values retain provider identity. They can live inside typed
collections. They cannot become native argv/stdin text, durable vocabulary or
JSON files. Sending one to a different provider or re-registering its provider
does not transfer ownership. Providers validate their own remote sessions and
object lifetimes. `help` reads provider metadata without invoking the subject.

## Optional native providers

```seed
#!/usr/bin/env hirari
capability register-library -example "C:/Providers/example.dll" "Example provider"
provide -example "operation" (data list 1 2 3)
capability unregister -example
```

Native libraries export this C ABI, with UTF-8 JSON request/response bytes:

```cpp
const char* capability_invoke_v1(const char*, size_t, size_t*);
void capability_shutdown_v1(); // optional
// Optional cooperative cancellation, preferred when exported:
const char* capability_invoke_cancellable_v1(
    const char*, size_t, size_t*, int (*cancelled)(void*), void*);
```

Request: `{"protocol":"hirari.values/1","arguments":[TAGGED_VALUES...]}`.
Response: the same protocol plus `value: TAGGED_VALUE`, or `error: TEXT`.
The library owns response memory until its next invocation. Calls are serialized
on the shell thread; no Seed pointer or C++ allocator crosses the ABI. Envelopes
are limited to 16 MiB. Shutdown must stop provider-owned activity before unload.
These libraries execute inside Hirari's process and share its crash boundary.
Use `capability register -example "example.exe"` for the process-provider route.

## Jobs

```seed
#!/usr/bin/env hirari
data bind -work (job start "worker.exe" (data list "--prepare"))
data bind -id (data get work "id")
job status id
# Other commands remain available while the worker runs.
job cancel id
```

The optional third argument to `start` is a structured options record, loaded
through a provider/configuration program or `data read`. Fields:

| Field | Contract |
| --- | --- |
| parameters | JSON data sent to the worker; defaults to an empty record. |
| structured | Require a structured result; defaults false. |
| timeout_ms | 1–86400000; defaults 300000. |
| slot, revision | A new request supersedes the previous request for that slot. |
| cache_directory, cache_key | Opt-in cache, requiring explicit immutable input/code digests in the key. |

Each worker gets one JSON request on stdin, followed by EOF:
`{"protocol":"hirari.jobs/1","request_id":"...","revision":"...","parameters":{}}`.
It may emit newline-terminated JSON records on stdout with that protocol and
request ID: `kind:"progress", value:...`; `kind:"result", value:...`; or
`kind:"error", message:"..."`. Wrong request IDs and duplicate results fail.
Other native stdout remains available. stderr and exit status are preserved.
Without structured mode, successful ordinary stdout is the result.

`status` exposes state, result/error, progress, stdout/stderr, exit status,
elapsed_ms, cache_hit and current. Superseded jobs expose no usable result.
`list` observes retained jobs. `forget ID` requires a stopped job.

Jobs capture their launch working directory. They are owned by this shell and
their Windows Job Objects prohibit breakaway; cancellation and shell shutdown
stop their process trees. There are at most 8 active and 256 retained jobs.
Process capture is bounded to 16 MiB; worker protocol lines and parameters to
1 MiB. Large artifacts belong in files referenced by the result.

Only successful current results enter the cache. Publication is atomic; corrupt
entries are misses. The key also includes executable path, argv, cwd, parameters
and revision. A path is not a content digest: the caller supplies content/code
hashes and must only cache reproducible work. Jobs are session-owned, not a
durable service manager, renderer scheduler or checkpoint system.
