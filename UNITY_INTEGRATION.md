# Optional renderer integration

Hirari is a general-purpose shell. Renderer protocols and clients are maintained
by the renderer, outside Seed and Hirari's compiled sources.

Skunkworks' optional client and its migration contract are documented at
[Skunkworks clients](https://github.com/JinkaiDesu/skunkworks/blob/main/docs/clients.md).
Register it through the generic capability API, or use its stdin/stdout CLI from
any shell. No rendering engine is required to build or use Hirari.
