# Assets

Engine and sample assets. Paths passed to loaders such as
`EgeModel::createModelFromFile` are resolved relative to this directory via the
`EGE_ASSET_ROOT` compile definition, so they do not depend on the working
directory the executable is launched from.

| Directory | Contents |
|---|---|
| `models/` | Mesh source files (OBJ today, glTF from Phase 4) |
| `materials/` | Native `.egematerial` files, loaded through the reflection-driven serializer |

Every importable file gets a `.egameta` sidecar holding its GUID. The sidecars
are committed alongside their assets on purpose: the id is what scene files
reference, so it has to be the same in a fresh checkout as it was in the one the
scene was saved from.

The engine watches this directory while it runs. Saving a change to
`materials/floor.egematerial` repaints the demo's floor without a restart.
