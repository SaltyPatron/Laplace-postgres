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
`laplace_labels` uses that reader for selected-ID display: Unicode text and
ordered tuples with their runs. It retains canonical IDs for other structural
roles or missing realizations, and does not reconstruct a package body to label
the package. Labels remain output; they are never lookup or traversal keys.

Containment provenance prepares a DAG's adjacency and topological order once
for all requested witness roots. Root-local path counts use that shared plan
and skip unreachable branches; games, signed outcomes, dependence roots and
attribution retain their existing Native meanings.

`laplace_present_ids` accepts a complete client ID frontier. Native routes the
IDs through the canonical range partitions and probes sorted arrays in their
primary-key indexes, returning stored IDs and their original ordinals,
including duplicate input occurrences. It checks parent-column SELECT
permissions and rejects active row-level security. Virtual Tier-0 membership
remains the client's perf-cache responsibility.

Containment frontiers, explicit root/claim selections and series-carrier
validation reuse that primary-key kernel. Only compact ID keys are sorted;
Native visits their range indexes in ID order and consumes paths directly.
This avoids a cardinality-dependent SQL join choosing full physicality scans
and a multi-gigabyte external sort. Path/mask column privileges and the shared
relation's RLS boundary are checked before direct access. Tuple detoasting is
released as each carrier is consumed; graph state remains transaction-owned.

A real ISO witness read on 2026-10-26 returned 93,441 series in 1.55 seconds
with the candidate, versus 70.91 seconds through the previous containment path.
A complete comparison of all returned columns found zero differences. The
profile attributed 68.94 seconds of the old path to database frontier fetches;
large frontiers scanned all 256 physicality partitions and externally sorted
3.49 GiB. These are populated-database containment measurements, not parsing,
COPY, publication or whole-source ingest throughput.

`laplace_series_snapshot` and `laplace_stage_snapshot` share one Native batch
calculation: carrier validation, witness registry, transitive lineage,
containment testimony and the existing series solver. The stage API keeps the
changed claim set in PostgreSQL. Explicit claim selections traverse upward
directly; unselected source-trunk enumeration traverses downward. No speculative
size threshold selects between them.

`laplace_apply_series` accumulates the client's known new/previously staged
series, publishes the stage-selected snapshot and consumes the delta in the
caller's transaction. `laplace_attest_stage` and the array adapter
`laplace_attest_series` use the same Native calculation and set-based
UPDATE/INSERT/DELETE publication. They preserve cumulative replay, signed trust,
dependence and removal of claims without evidence, without fixed claim slices
or conflict-driven accumulation. Source registration retains its transaction
boundary; these APIs do not independently commit it.

`laplace_merge_stage` drains GIN pending entries for copied path partitions
before consuming the staging tables and returning to provenance/series reads.
`laplace_clean_stage` routes the copied IDs through the same Native partition
bounds as enrichment and uses the existing restricted index-maintenance
authority. It leaves unrelated partitions alone. Search results are unchanged;
subsequent containment reads avoid repeatedly scanning the bulk insert lists.

The merge consumes paired entity/path inputs from one formed batch. Native
clients carry a boolean `present` classification with each staged row. Positive
classifications are retained; only claimed-new IDs receive the flat concurrent
overlap verification, through the same Native indexed membership kernel used by
tier descent. Plain inserts consume the absent set, and the Native updater
consumes existing metadata. There is no global structural apply lock or
destination anti-join. A genuine canonical primary-key race rolls back the
attempt and re-verifies a strictly smaller remaining set; other failures surface.

Existing IDs may omit coordinates, Hilbert keys and unchanged paths. A supplied
path carries incoming annotations for Native enrichment; omitted fields preserve
the recorded content. Clients without classifications use an ID-only write plan
through the same verifier and publication statements. That lower-level import
interface does not replace the ingest spine's client-side novelty descent.

## CI installation boundary

Native owns function binding contracts in `manifest/function_bindings.tsv`.
The compiler call-graph check verifies that registry against all exported C
entry points. `laplace_restore_function_bindings()` restores it on existing
installations: ROM/geometry/index intrinsics run directly, SQL/SPI entry points
retain their namespace scope, and thin SQL adapters use parsed `BEGIN ATOMIC`
bodies whose references bind at definition time. Consumers call this owner
instead of assigning settings to every extension function.

Frozen bank and value names bind once per expression; varying arguments are
checked before reusing that binding. Existing-carrier updates always use the
partition-routed copied ID set. There is no statistics-selected whole-table
update alternative.

Pull-request CI builds a candidate and runs `tests/extension-install.sh` against a private `DESTDIR` package and a newly created test database. PostgreSQL 18's `extension_control_path` selects that package. Only its private control file is rewritten to reference the candidate module; an assertion checks every extension C function's module binding. The test executes `laplace_isa()` and removes its database and package. Failed database cleanup preserves the package and fails the job.

PR CI does not install into shared PostgreSQL directories or migrate the declared application database. Shared installation and extension upgrades belong to Operations deployment after merge. This boundary uses PostgreSQL's documented [extension search paths](https://www.postgresql.org/docs/18/runtime-config-client.html); the Windows installation path has not been qualified by this Linux test.
