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

`CREATE EXTENSION laplace` makes the four tables (`entity`, `physicality`, `witness`, `consensus`), their partitions, their indexes and the lookups, as PostGIS makes `spatial_ref_sys`; their data goes with a dump. `physicality.mask` holds what a row is and the types it holds, indexed with the path's constituents in one GIN (`laplace_path_ops`, `laplace_mask_ops`). The highway (`laplace.highway`) gives a type's slot, bit and mappings in place. `laplace_schema_indexes()` makes every index again if one was dropped.

Provenance is containment (`LP_SAID_RECORD`, `LP_SAID_CLAIM` vertices under each file's trunk in `physicality`), walked natively in C by `laplace_attested(claims blake3[], scope blake3[] DEFAULT '{}')` and aggregated into Glicko-2 Fisher-information basis-point shares (`share_bp` summing to `10000`) by `laplace_attribution(claims blake3[], scope blake3[] DEFAULT '{}')`.

Native set reads own claim acquisition and realization. `laplace_claims` and
`laplace_claims_each` accept the caller's fan, or SQL `NULL` for all responders;
virtual Tier-0 selectors do not require stored entity rows. `laplace_text_batch`
realizes an ordered set of selected IDs in one context, preserves duplicate input
occurrences and compressed runs, and reads shared descendants once. Scalar
`laplace_text` uses the same reader. Newly discovered nodes form each frontier;
transfer windows follow `work_mem` and PostgreSQL's actual array capacity.
There is no separate 64-level text rejection. Cyclic carriers are rejected and
the server's stack/resource checks still apply.

Containment provenance prepares a DAG's adjacency and topological order once
for all requested witness roots. Root-local path counts use that shared plan
and skip unreachable branches; games, signed outcomes, dependence roots and
attribution retain their existing Native meanings.

## CI installation boundary

Pull-request CI builds a candidate and runs `tests/extension-install.sh` against a private `DESTDIR` package and a newly created test database. PostgreSQL 18's `extension_control_path` selects that package. Only its private control file is rewritten to reference the candidate module; an assertion checks every extension C function's module binding. The test executes `laplace_isa()` and removes its database and package. Failed database cleanup preserves the package and fails the job.

PR CI does not install into shared PostgreSQL directories or migrate the declared application database. Shared installation and extension upgrades belong to Operations deployment after merge. This boundary uses PostgreSQL's documented [extension search paths](https://www.postgresql.org/docs/18/runtime-config-client.html); the Windows installation path has not been qualified by this Linux test.
