# Assets

Engine and sample assets. Paths passed to loaders such as
`EgeModel::createModelFromFile` are resolved relative to this directory via the
`EGE_ASSET_ROOT` compile definition, so they do not depend on the working
directory the executable is launched from.

| Directory | Contents |
|---|---|
| `models/` | Mesh source files (OBJ today, glTF from Phase 4) |
