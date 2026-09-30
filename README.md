# Laplace-postgres

Laplace's 4D expansion of PostgreSQL and PostGIS: identity, physicality paths, the GIN key that finds containers, continuations, 4D distance, the shape measures, Hilbert order, the wall, an exact 4D centroid aggregate, a standing's confidence, and a text's entity (its ID, tier, coordinate and constituents) computed in place from the memory-mapped tier-0 perf-cache. Standard geometry types are used as they are; the math is Laplace-Native, built in.

`sql/` also holds the content schema (`schema.sql`), the semantics (`semantics.sql`), the lookups ingestion needs (`lookup.sql`), and the indexes built after a bulk load (`indexes.sql`). [Laplace-Engine](https://github.com/SaltyPatron/Laplace-Engine) runs them: `laplace deploy`, `laplace index`. The documentation is [Laplace-Wiki](https://github.com/SaltyPatron/Laplace-Wiki), published at <https://saltypatron.github.io/Laplace-Wiki/>.

## Build and install

```sh
source ../Laplace-Engine/laplace.env
cmake --preset icx-release && cmake --build --preset icx-release && cmake --install $LAPLACE_BUILD/Laplace-postgres/icx-release
laplace deploy                    # extensions, schema, semantics, the database's tier 0
laplace ingest files...
laplace index
python3 bench/bench_queries.py    # query benchmark: cold and warm execution, buffers, partitions touched
```

Change the extension through versioned upgrade scripts (`ALTER EXTENSION laplace UPDATE`, which `laplace deploy` runs), never by dropping it: indexes such as the GIN container index depend on its functions.
