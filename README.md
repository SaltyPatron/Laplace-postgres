# Laplace-postgres

Laplace's 4D expansion of PostgreSQL and PostGIS: identity, physicality paths, the GIN key that finds containers, continuations, 4D distance, the shape measures, Hilbert order, the wall, an exact 4D centroid aggregate, a standing's confidence, and a text's entity (its ID, tier, coordinate and constituents) computed in place from the memory-mapped tier-0 perf-cache. Standard geometry types are used as they are; the math is Laplace-Native, built in.


## Build and install

```sh
source ../Laplace-Engine/laplace.env
cmake --preset icx-release && cmake --build --preset icx-release && cmake --install $LAPLACE_BUILD/Laplace-postgres/icx-release
laplace deploy                    # extensions, schema, semantics, the database's tier 0
laplace ingest files...
laplace index
python3 bench/bench_queries.py    # query benchmark: cold and warm execution, buffers, partitions touched
```

On Windows (icx, against an icx-built PostgreSQL), `cmake --install` also puts the DLLs `laplace.dll` loads next to `postgres.exe`, since `LoadLibrary` does not search PATH: ICU's (`icuuc78`, `icudt78`) and Intel's `libmmd`, the list read from the module's imports at install time.

Change the extension through versioned upgrade scripts (`ALTER EXTENSION laplace UPDATE`, which `laplace deploy` runs), never by dropping it: indexes such as the GIN container index depend on its functions.

## The schema

`CREATE EXTENSION laplace` makes the four tables (entity, physicality, witness, consensus), their partitions, their indexes and the lookups, as PostGIS makes `spatial_ref_sys`; their data goes with a dump. `physicality.mask` holds what a row is and the types it holds, indexed with the path's constituents in one GIN (`laplace_path_ops`, `laplace_mask_ops`). The highway (`laplace.highway`) gives a type's slot, bit and mappings in place. `laplace_schema_indexes()` makes every index again if one was dropped.

## CI installation boundary

Pull-request CI builds a candidate and runs `tests/extension-install.sh` against a private `DESTDIR` package and a newly created test database. PostgreSQL 18's `extension_control_path` selects that package. Only its private control file is rewritten to reference the candidate module; an assertion checks every extension C function's module binding. The test executes `laplace_isa()` and removes its database and package. Failed database cleanup preserves the package and fails the job.

PR CI does not install into shared PostgreSQL directories or migrate the declared application database. Shared installation and extension upgrades belong to Operations deployment after merge. This boundary uses PostgreSQL's documented [extension search paths](https://www.postgresql.org/docs/18/runtime-config-client.html); the Windows installation path has not been qualified by this Linux test.
