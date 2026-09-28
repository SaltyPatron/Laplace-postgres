# Laplace-postgres

Laplace's 4D expansion of PostgreSQL and PostGIS: identity, physicality paths, the GIN key that finds containers, continuations, 4D distance, discrete Fréchet, Hilbert order, the wall, an exact 4D centroid aggregate, and coordinates computed in place from the memory-mapped tier-0 perf-cache. Standard geometry types are used as they are; the math is Laplace-Native, linked in.

## Build and install

```sh
source /repos/src/toolchain.env; export PATH=$HOME/.local/bin:$PATH
cmake --preset icx-release && cmake --build --preset icx-release && cmake --install /repos/build/Laplace-postgres/icx-release
psql -c "CREATE EXTENSION postgis; CREATE EXTENSION laplace"
psql -f sql/schema.sql            # the partitioned content schema
laplace-ingest files...           # from Laplace-Native
psql -f sql/indexes.sql
python3 bench/bench_queries.py    # query benchmark: cold and warm execution, buffers, partitions touched
```

Change the extension through versioned upgrade scripts (`ALTER EXTENSION laplace UPDATE`), never by dropping it: indexes such as the GIN container index depend on its functions.
