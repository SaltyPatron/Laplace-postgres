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

The linked module is also published as `laplace_<content hash>.dll` / `.so`.
The generated control file selects that generation, and the in-place update
rebinds existing C functions without changing their OIDs, attributes or indexes.
This lets long-lived PostgreSQL backends load the new implementation after an
update. Package the generated control file alongside its matching module;
the stable `laplace` library remains available for predecessor installations.

## The schema

The installation source is `sql/laplace.sql.in`: one current baseline.
`sql/operations.sql` defines the publication and maintenance operations once;
CMake includes it in both the baseline and the in-place 1.38–1.55-to-1.56 development
transitions from one update template. Installable scripts are generated under the build's
`extension/` directory. Cold installs do not replay earlier development revisions.

`CREATE EXTENSION laplace SCHEMA chosen_schema` binds the native substrate to
that namespace; PostGIS may be installed in a different namespace. The C binding
owner resolves both installed extension namespaces and invalidates cached names
and plans after catalog changes. Ordinary functions do not set `search_path`.
Parsed SQL adapters bind references at definition time. A dedicated database
may use `public`; installation alongside unrelated tables uses a chosen schema.
Functional API schemas remain separate from the storage namespace.

Private ingestion tables carry a load ownership marker installed by
`laplace_stage_own(token, tables)`, using the `lp_<purpose>_<token>[_<lane>]`
private-table convention. The token identifies the load operation; it is not
a content or witness identity. The owning control connection retains a
PostgreSQL session advisory lock across COPY transactions. A new ingest or
`laplace-ops clean` calls `laplace_stage_retire()` to reclaim marked tables only
after that connection exits. Idle live loads remain protected, and unmarked
staging tables are preserved. Table creation and ownership registration commit
together; normal successful publication removes the tables in its transaction.

`laplace_accumulate_series(pending, delta)` adds window-local observations to
private UNLOGGED load tables. It retains the carried win numerator alongside
games, keeping means independent of physical window boundaries. Each window
commits without touching public consensus or acquiring its bucket locks. The
canonical source trunk and authoritative Native standings publish together;
an interrupted source leaves reusable structural closure and owned private
tables for normal lifecycle cleanup, without a fabricated witness.

`CREATE EXTENSION laplace` makes the four tables (`entity`, `physicality`, `witness`, `consensus`), their partitions, their indexes and the lookups, as PostGIS makes `spatial_ref_sys`; their data goes with a dump. `physicality.mask` holds what a row is and the types it holds, indexed with the path's constituents in one GIN (`laplace_path_ops`, `laplace_mask_ops`). The highway (`laplace.highway`) gives a type's slot, bit and mappings in place. `laplace_schema_indexes()` makes every index again if one was dropped.

Provenance is containment (`LP_SAID_RECORD`, `LP_SAID_CLAIM` vertices under each file's trunk in `physicality`). `laplace_attested(claims blake3[], include_files boolean DEFAULT false)` returns actual source/container testimony. `laplace_attribution(claims blake3[])` adds per-claim source shares. `laplace_source_attribution(claims blake3[], scope blake3[] DEFAULT '{}')` returns model/query settlement across distinct selected claims: source ID, claims touched, exact bigint games, Fisher information and basis points. Empty scope selects all sources; a supplied scope filters witnesses before solving.

`laplace_source_standings(claims blake3[], scope blake3[] DEFAULT '{}')` exposes that same selected-source solve directly to path/query consumers, returning one standing per distinct witnessed claim.

Attribution uses `lp_series_evaluate`, the same rooted posterior that produces standings. A shared dependence root contributes `J/(1+omega²J)` once, distributed among its observations by games; repeated copies cannot manufacture independent precision. Registered lineage and explicit dependence qualifiers use the same resolution and signed-trust semantics as cumulative ingestion. Largest-remainder settlement happens after source aggregation, with canonical source-ID ties and exactly `10000` basis points for positive total information. Zero-information witnesses retain their testimony and receive zero shares. Indexed witness metadata, containment and realization use the caller's snapshot and column privileges; these read paths require no SPI scope or schema setting.

Native set reads own claim acquisition and realization. `laplace_claims` and
`laplace_claims_each` accept the caller's fan, or SQL `NULL` for all responders;
virtual Tier-0 selectors do not require stored entity rows. `laplace_text_batch`
realizes an ordered set of selected IDs in one context, preserves duplicate input
occurrences and compressed runs, and reads shared descendants once. Scalar
`laplace_text` uses the same reader. Newly discovered nodes form each frontier;
transfer windows follow `work_mem` and PostgreSQL's actual array capacity.

`laplace_claims_each(..., endpoint, middle_any, arity, opposite)` intersects
both directed endpoint sets in the shared Native access. `endpoint=1`
selects input first constituents and `opposite` last constituents; `endpoint=2`
reverses those roles. NULL leaves the opposite endpoint unconstrained, while
an empty array selects nothing. An opposite set with no more distinct IDs than the primary set joins the GIN
scan; broader sets qualify acquired carriers before standing lookup. Both sets
use bounded complete transfer windows, and every carrier qualifies against both
complete sets, including lossy bitmap rechecks, before standing lookup and fan.
The seven-argument overload retains its existing behavior and needs no index
rebuild.

`laplace_parts(geometry)` projects ordered logical IDs from an already acquired
path using the Native run decoder. It retains repetitions and performs no
database lookup. Query adapters use this overload rather than fetching the
same carrier again by ID; `laplace_parts(text)` retains its text decomposition
contract, and `laplace_vertex_ids` retains its distinct sorted index-key contract.

Text and label classification/rendering use explicit Native occurrence stacks,
preserving child order and runs without a fixed altitude cutoff or recursive C
calls. Cyclic carriers are rejected; output and working memory remain subject to
the server's resource limits. Regression realizes 4,096 genuine nested tiers
with a 100 kB backend stack budget, repeated roots and deliberate cycle rejection.
`laplace_labels` uses that reader for selected-ID display: Unicode text and
ordered tuples with their runs. It retains canonical IDs for other structural
roles or missing realizations, and does not reconstruct a package body to label
the package. Labels remain output; they are never lookup or traversal keys.

`laplace_claim_preview` opens a selected claim set through that same reader and
batch-realizes its constituents once. Ternary predicates and binary/ternary
objects retain their IDs; n-ary claims retain their own object identity and
complete ordered, counted preview. It preserves duplicate request ordinals,
omits missing/non-claim roots, and does not expand RLE runs into occurrence
arrays. Host relation metadata remains a projection over the returned IDs.

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

`laplace_entities(ids)` and `laplace_physicalities(ids)` project complete stored
rows through this same primary-key kernel. They return one row per stored input
occurrence with its one-based request ordinal; missing IDs have no row. Empty
requests return no rows and NULL members are rejected. Entity rows retain tier,
coordinate, Hilbert address and mask; physicality rows retain tier, Hilbert
address, the exact ordered/RLE carrier and mask. Row order is unspecified: use
the ordinal to associate results with inputs. The active caller snapshot,
parent/column privileges and RLS boundary apply. Clients never need partition
names or extra connections for these reads.
`laplace_standings(ids)` uses the same indexed kernel and request ordinals
for rating, deviation, volatility and signed 64-bit match counts.

`laplace_holders(parts, required, per_part, floor, cap, kinds)` performs direct
overlap containment in the same Native GIN owner. `required` adds all-member
conditions. Union mode emits each parent once with ordinal zero; per-part mode
retains one-based input ordinals, including duplicates. `floor` is exclusive;
`kinds` selects claims (1), observations (2), or both (3). With `cap=-1` the
sets are complete. A nonnegative cap is per input and kind: equality returns
the complete set; overflow emits one `hub=true` row with a null carrier and
no arbitrary subset. A single work_mem-bounded spool holds tentative results,
spilling through PostgreSQL when needed. No per-row or per-partition SPI is
used, no extra connections are opened, and no planner settings are changed.

`laplace_claims` and `laplace_claims_each` use that same GIN/primary-key kernel
for scored responders. Native applies mask, middle-refusal and tier rules,
reads consensus as an exact ID set, then elects responders in ID order.
Qualification precedes the per-input fan; NULL fan returns the complete set.
Duplicate input ordinals, compressed runs and 64-bit counts are preserved.
The functions perform no SPI planning over the partitioned relations.

Partition bounds route keys, and PostgreSQL's tuple-conversion map projects
child storage into the parent's declared columns. Renaming or reordering a
partition's physical columns cannot change the returned ID, tier or carrier.

Containment frontiers, explicit root/claim selections and series-carrier
validation reuse that primary-key kernel. Only compact ID keys are sorted;
Native visits their range indexes in ID order and consumes paths directly.
This avoids a cardinality-dependent SQL join choosing full physicality scans
and a multi-gigabyte external sort. Path/mask column privileges and the shared
relation's RLS boundary are checked before direct access. Tuple detoasting is
released as each carrier is consumed; graph state remains transaction-owned.

Upward containment uses the canonical GIN indexes directly. PostgreSQL's bitmap
table access applies MVCC visibility. When its memory budget requires page-level
instead of tuple-level candidates, Native rechecks each candidate path against
the exact 128-bit ID set. This bitmap representation never approximates IDs or
discards true matches. Duplicate parent IDs are reduced before the shared
primary-index reader visits them in ID order. Each partition's bitmap follows
`work_mem`; array windows follow the same budget and PostgreSQL's representation
limits. Large selections stream the GIN membership-key range against the owned
Native ID set; small selections seek individual entries. Neither switches to a
physicality heap scan. Regression covers 120,000 deliberately absent lookup IDs,
positive dense matches, and false-candidate rejection under a 64 kB budget.
No planner setting or unknown SPI-array selectivity determines access.
Parent/column ACLs, active RLS rejection, index validity and
snapshot safety remain authoritative. Exact provenance rejects nonzero
`gin_fuzzy_search_limit`; it cannot accept approximate index results.

`laplace_containers(parts, bits)` uses that same GIN/primary-key owner for
all-member reads, with mask filtering and original physicality carriers.
Its GIN nominations use all requested keys. When an input spans several array
windows, candidate paths are rechecked against the complete set before their
IDs are merged. Duplicate keys retain set semantics. Unstored basis children
use the zero floor, so a missing `entity` row cannot turn a tier comparison
into NULL and hide a valid parent. The operator performs no SPI query and
uses the caller's snapshot and access policy. Native ID maps are released on
both successful reads and PostgreSQL errors.

A 2026-10-27 read selected 10,000 claims from 20.3 million physicalities.
The installed reader's first two frontiers each scanned all 256 physicality
partitions. In one repeatable-read snapshot, the installed reader returned
14,011 provenance rows in 67.24 seconds; direct GIN returned the identical
complete-row digest in 45.54 seconds. This ran alongside ingest; it is a
populated containment comparison, not an isolated whole-source throughput claim.

A later 100,000-claim comparison used two fresh backends at one exported
repeatable-read snapshot. The installed 1.40 reader returned 142,679 rows in
29.62 seconds; the 1.41 candidate returned the same complete-row digest in
11.94 seconds. These are containment-read timings, not whole-ingest throughput.

Perf-cache settings have one backend owner. Loaded module generations consult
that configuration without redefining its GUCs or keeping divergent private
values. Regression coverage loads a second module against the same settings.

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

The two-argument `laplace_merge_stage` drains GIN pending entries for copied
path partitions before consuming staging tables and returning. Native clients
can pass a third `true` argument to defer maintenance across closed dependency
waves. Each merge returns its actual destination partition OIDs; the client
unions these per writer and calls `laplace_clean_partitions` once per working
window, before provenance/series reads. Child-wave commit barriers and source
publication remain unchanged. Older clients retain immediate maintenance.
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

## Shared database interfaces

The lower layers are inventoried in [Native's interface map](../Laplace-Native/README.md#shared-interfaces-from-codepoints-upward).
This extension adds PostgreSQL access and result adapters above those kernels.

Large SQL overlap selections use GIN's [partial-match interface](https://www.postgresql.org/docs/18/gin.html#GIN-PARTIAL-MATCH)
to stream the membership-key range against one Native ID set. Native containment
chooses streamed matching for a small dictionary and exact entry seeks for a
sparse frontier in a larger dictionary. An internal ScanKey strategy requests
exact postings for the complete memory-bounded array, without splitting at the
SQL operator's range-switch boundary. Each leaf opens once, unions every transfer
window into one bitmap, and visits its heap once. PostgreSQL owns pending lists, posting trees,
bitmap visibility and concurrency in both cases. No cardinality threshold
switches a frontier to a physicality heap scan. Existing index keys retain all
128 ID bits and their endpoint role, so this support-method update needs no
index rebuild. Index key contexts lease an owned set; exact operand bytes allow
partition scans to share it. One idle set is retained until transaction end,
and replaced sets are freed when their last reader releases them. No rows or
snapshot-dependent answers are cached. Regression checks actual transaction heap-scan counters, exact dense and
lossy results, and large inputs with matching keys in pending lists.
Cold installation registers the complete support-method list before creating
indexes. A same-backend custom-schema regression prevents index initialization
from caching an incomplete list and silently treating set matches as one key.

Upward provenance retains the complete requested ID frontier but creates DAG
nodes only for actual assertion edges. Missing or unasserted operands cannot
contribute evidence and do not become thousands of empty reduction roots.

| Boundary | Owner | Shared inputs and variation |
| --- | --- | --- |
| Installed object binding | `src/binding.c`: `lp_pg_schema/relation/sql` | Catalog-derived namespace; SQL templates use `@extschema@` / `@postgis@` |
| Indexed identity sets | `src/enrichment.c`: `indexed_keys`, `lp_pg_present` | Complete ID set, canonical relation, original ordinals, result callback |
| Downward path frontier | `lp_pg_lookup_paths` | Complete ID set, whether masks are needed, path callback and context |
| Executor result stream | `lp_pg_stream` | Prepared SELECT, typed parameters, row callback and context |
| Realization | `read_levels`, `realize_batch` | Shared discovered-ID map; exact text or display-label policy |
| Provenance and series | `src/provenance.c` | Shared containment edges, runs and rooted reduction; explicit claims, roots and witness scope |
| SQL API adapters | Parsed `BEGIN ATOMIC` bodies | Typed arguments and output projection; Native owns repeated computation |

Callbacks borrow a tuple/path for that invocation. Their row memory is reset
after consumption; retained state must be copied into the operation's context.
Native maps and buffers use the host allocator. Indexed path access checks the
parent relation's requested columns and rejects active RLS rather than bypassing
it. Missing IDs are omitted from path/presence set results; original ordinals
preserve duplicate requests. Exact text requires every path, while labels may
retain the canonical ID when a realization is unavailable. Transfer windows
bound working memory and do not cap admitted input or DAG depth.

The executor receiver itself lives in `include/laplace_pg/executor.h`, exposed
by the `laplace_pg_executor` CMake interface. Both the canonical module and the
application extension compile this owner. Their adapters supply parameters and
snapshot policy; neither maintains a separate portal/fetch implementation.

Use typed data, a callback and its context where mechanics are shared. Keep
storage authorization, missing-value policy and domain meaning explicit; a
single mode flag must not silently exchange those contracts. SIMD selection
belongs to Native kernels rather than SQL wrappers or schema-specific copies.

## CI installation boundary

Native owns function binding contracts in `manifest/function_bindings.tsv`.
The compiler call-graph check verifies that registry against all exported C
entry points. `laplace_restore_function_bindings()` restores it on existing
installations by removing per-function search-path settings from ordinary
functions. ROM/geometry/index intrinsics run directly, SQL/SPI entry points
bind installed objects explicitly, and thin SQL adapters use parsed
`BEGIN ATOMIC` bodies. Consumers call this owner instead of assigning settings
to every extension function. The registry records whether a C entry point uses
SPI; that classification does not grant authority or require a GUC scope.

Frozen bank and value names bind once per expression; varying arguments are
checked before reusing that binding. Existing-carrier updates always use the
partition-routed copied ID set. There is no statistics-selected whole-table
update alternative.

Pull-request CI builds a candidate and runs `tests/extension-install.sh` against a private `DESTDIR` package and a newly created test database. PostgreSQL 18's `extension_control_path` selects that package. Only its private control file is rewritten to reference the candidate module; an assertion checks every extension C function's module binding. The test executes `laplace_isa()` and removes its database and package. Failed database cleanup preserves the package and fails the job.

PR CI does not install into shared PostgreSQL directories or migrate the declared application database. Shared installation and extension upgrades belong to Operations deployment after merge. This boundary uses PostgreSQL's documented [extension search paths](https://www.postgresql.org/docs/18/runtime-config-client.html); the Windows installation path has not been qualified by this Linux test.

`laplace_ordered_pair_counts(vocab,lo,hi,roots,separators)` projects immediate
ordered child occurrences from the canonical registered DAG at every tier.
`roots=NULL` selects all registered witnesses; an explicit array selects a subset.
Shared containers are visited once, even beneath repeated or multiple roots;
unregistered branches are excluded. Native run arithmetic retains exact RLE
multiplicities without expansion. `vocab` selects subjects, retaining all objects
for the caller's normalization denominator. Empty vocabulary or roots select no
rows; NULL vocabulary admits every subject. This API describes stored structure,
not flattened document tokens or additional testimony. Consumers aggregate the
returned `(container,gap,subject,object,occurrences)` with PostgreSQL spillable
operators. `laplace_separator_ids()` reads Unicode Zs/Zl/Zp/Cc and CRLF from the
mapped floor, independent of corpus admission. Passing NULL separators disables
filtering. No API changes caller planner or search-path settings.

The same CMake target can be embedded after an existing Laplace-Native target.
Set `LAPLACE_PG_CONFIG` to the target server and `LAPLACE_PG_INSTALL_LIBDIR`,
`LAPLACE_PG_INSTALL_SHAREDIR`, `LAPLACE_PG_INSTALL_BINDIR` to the product package
destinations. `LAPLACE_PG_MODULE_PATHNAME` selects its installed module binding;
`@MODULE_NAME@` expands to the linked library's content-addressed basename.
The default is `$libdir/laplace_<digest>`; products with a private
`dynamic_library_path` can select `@MODULE_NAME@` alone. The monorepo builds and installs this
canonical target with its consumers, including the generated baseline and
upgrade scripts. It does not depend on a separately installed extension version.

The five-argument `laplace_claims_each(ids, fan, bits, refuse, endpoint)` adds
first (`1`), last (`2`), or either (`3`) endpoint selection; `0` preserves the
four-argument membership behavior. Both signatures share one Native reader.
Endpoint selection unions existing GIN first/last postings in bounded windows,
then qualifies masks, tier, refusal and actual standings before the per-input
fan. Duplicate request ordinals and exact lossy bitmap rechecks are preserved.
The 1.49 baseline and direct development update add this operation without
rewriting data or rebuilding indexes.

Positive carrier-mask requirements join path and endpoint keys inside the existing
multicolumn GIN scan. Claims, all-member containers and claim-only holder probes
therefore intersect postings before fetching heap rows. Exact carrier checks
remain authoritative for lossy pages and caller-owned path-only indexes.

The six-argument `laplace_claims_each(..., endpoint, middle_any)` additionally
requires one of the supplied IDs in the path's logical middle. SQL NULL leaves
that constraint open; an empty array selects nothing. The existing GIN postings
intersect endpoint, required-member and positive carrier-mask keys together;
Native rechecks actual middle membership (including RLE) before the caller's fan.
The 1.50 baseline and direct installed-development transition add this overload
without rewriting retained data or indexes; the four- and five-argument ABIs remain.

An optional final `arity bigint` selects the exact logical constituent count,
including RLE repetitions; zero or SQL NULL leaves shape unconstrained. This
qualification runs before primary-key standing acquisition, not after all
matching carriers have crossed into a SQL function result. Binary-relation
projections pass `3`; n-ary and value-claim consumers retain their existing shape.
