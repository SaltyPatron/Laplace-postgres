-- Current operation definitions, shared by cold install and development update.
CREATE OR REPLACE FUNCTION @extschema@.blake3_from_bytea(bytea) RETURNS @extschema@.blake3
    AS 'MODULE_PATHNAME','blake3_from_bytea' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION @extschema@.blake3_from_bytea(bytea)
    IS 'Convert a 16-byte raw bytea identity directly into a Native blake3 identity without hex encoding.';
DO $blake3_casts$ BEGIN
 IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_cast WHERE castsource = 'bytea'::regtype AND casttarget = '@extschema@.blake3'::regtype) THEN
  CREATE CAST (bytea AS @extschema@.blake3) WITH FUNCTION @extschema@.blake3_from_bytea(bytea) AS ASSIGNMENT;
 END IF;
 IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_cast WHERE castsource = '@extschema@.blake3'::regtype AND casttarget = 'bytea'::regtype) THEN
  CREATE CAST (@extschema@.blake3 AS bytea) WITH FUNCTION @extschema@.blake3_send(@extschema@.blake3) AS ASSIGNMENT;
 END IF;
END $blake3_casts$;
COMMENT ON FUNCTION @extschema@.laplace_path_has_roles(@extschema:postgis@.geometry,smallint)
    IS 'Whether a canonical POINT/LINESTRING ZM path has any vertex whose stored said role is selected by the 8-bit mask. Bit 0 selects ordinary content; bits 1/2 select CLAIM/RECORD. Reads actual edge metadata, without expanding RLE runs or consulting child masks.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_parts(@extschema:postgis@.geometry) RETURNS @extschema@.blake3[]
    AS 'MODULE_PATHNAME','laplace_path_parts' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION @extschema@.laplace_parts(@extschema:postgis@.geometry)
    IS 'Ordered logical constituents of an already acquired Native path, including every RLE repetition. No entity or physicality lookup; unlike laplace_vertex_ids, this preserves order and duplicates.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_claims_each(ids @extschema@.blake3[],
    fan bigint, bits smallint[], refuse @extschema@.blake3[], endpoint smallint,
    middle_any @extschema@.blake3[], arity bigint DEFAULT 0, OUT i bigint, OUT entity @extschema@.blake3,
    OUT path @extschema:postgis@.geometry, OUT rating double precision,
    OUT deviation double precision, OUT volatility double precision, OUT matches bigint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 1024 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE AS 'MODULE_PATHNAME','laplace_claims_each';
COMMENT ON FUNCTION @extschema@.laplace_claims_each(@extschema@.blake3[],bigint,smallint[],@extschema@.blake3[],smallint,@extschema@.blake3[],bigint)
    IS 'The shared Native claim selector with a positive middle-member set: NULL is unconstrained, an empty set selects nothing. GIN intersects endpoint, positive mask and any required member before heap reads; Exact arity is optional (0 or NULL selects every shape); middle/RLE/arity qualification precedes standing fetch and fan. No index rebuild is needed.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_claims_each(ids @extschema@.blake3[],
    fan bigint, bits smallint[], refuse @extschema@.blake3[], endpoint smallint,
    middle_any @extschema@.blake3[], arity bigint, opposite @extschema@.blake3[],
    OUT i bigint, OUT entity @extschema@.blake3, OUT path @extschema:postgis@.geometry,
    OUT rating double precision, OUT deviation double precision, OUT volatility double precision,
    OUT matches bigint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 1024 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE AS 'MODULE_PATHNAME','laplace_claims_each';
CREATE OR REPLACE FUNCTION @extschema@.laplace_claims_each(ids @extschema@.blake3[],
    fan bigint, bits smallint[], refuse @extschema@.blake3[], endpoint smallint,
    middle_any @extschema@.blake3[], arity bigint, opposite @extschema@.blake3[],
    witnesses @extschema@.blake3[],
    OUT i bigint, OUT entity @extschema@.blake3, OUT path @extschema:postgis@.geometry,
    OUT rating double precision, OUT deviation double precision, OUT volatility double precision,
    OUT matches bigint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 1024 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE AS 'MODULE_PATHNAME','laplace_claims_each';
COMMENT ON FUNCTION @extschema@.laplace_claims_each(@extschema@.blake3[],bigint,smallint[],@extschema@.blake3[],smallint,@extschema@.blake3[],bigint,@extschema@.blake3[])
    IS 'The same directed Native claim selector with an opposite endpoint set. For endpoint 1 it restricts the last constituent; for endpoint 2 it restricts the first. NULL is unconstrained; an empty set selects nothing. The opposite set joins the GIN scan when its distinct ID count is no larger than the primary set; broader sets qualify acquired carriers in Native C. Complete transfer windows are unioned and both complete endpoint sets qualify every carrier before standing lookup and fan.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_claims_each(ids @extschema@.blake3[],
    fan bigint, bits smallint[], refuse @extschema@.blake3[], endpoint smallint,
    OUT i bigint, OUT entity @extschema@.blake3, OUT path @extschema:postgis@.geometry,
    OUT rating double precision, OUT deviation double precision, OUT volatility double precision,
    OUT matches bigint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 1024 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_claims_each';
COMMENT ON FUNCTION @extschema@.laplace_claims_each(@extschema@.blake3[],bigint,smallint[],@extschema@.blake3[],smallint)
    IS 'Native scored claims per input occurrence. endpoint: 0 any position, 1 first, 2 last, 3 either. Directed probes use existing GIN first/last postings with exact lossy rechecks; masks, refusals, tier and standing qualify before fan. No index rebuild is needed.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_source_standings(claims @extschema@.blake3[],
    scope @extschema@.blake3[] DEFAULT '{}', OUT claim @extschema@.blake3,
    OUT rating double precision, OUT deviation double precision, OUT volatility double precision,
    OUT matches bigint) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_source_standings';
COMMENT ON FUNCTION @extschema@.laplace_source_standings(@extschema@.blake3[],@extschema@.blake3[])
    IS 'Solve distinct selected claim standings from actual containment testimony and registered/explicit dependence roots. Empty scope selects all sources. A source scope is a typed input applied before solving, without session state or shadow tables.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_source_attribution(claims @extschema@.blake3[],
    scope @extschema@.blake3[] DEFAULT '{}', OUT source_id @extschema@.blake3,
    OUT claims_touched bigint, OUT games bigint, OUT fisher_weight double precision,
    OUT share_bp integer) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_source_attribution';
COMMENT ON FUNCTION @extschema@.laplace_source_attribution(@extschema@.blake3[],@extschema@.blake3[])
    IS 'Source royalties across distinct selected claims, from the same rooted posterior as standings. Empty scope selects all witnesses; otherwise scope is applied before solving. Dependence-root information saturates and is distributed by actual root observations. Positive information totals exactly 10000 basis points, with source-ID ties; zero information receives zero shares.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_holders(parts @extschema@.blake3[],
    required @extschema@.blake3[] DEFAULT '{}', per_part boolean DEFAULT false,
    floor smallint DEFAULT -1, cap bigint DEFAULT -1, kinds smallint DEFAULT 3,
    OUT ordinal bigint, OUT entity @extschema@.blake3, OUT path @extschema:postgis@.geometry,
    OUT tier smallint, OUT mask bigint, OUT kind smallint, OUT hub boolean) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_holders';
CREATE OR REPLACE FUNCTION @extschema@.laplace_holders(parts @extschema@.blake3[],
    required @extschema@.blake3[], per_part boolean,
    floor smallint, cap bigint, kinds smallint,
    bits smallint[], endpoint smallint DEFAULT 0,
    OUT ordinal bigint, OUT entity @extschema@.blake3, OUT path @extschema:postgis@.geometry,
    OUT tier smallint, OUT mask bigint, OUT kind smallint, OUT hub boolean) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_holders';
COMMENT ON FUNCTION @extschema@.laplace_holders(@extschema@.blake3[],@extschema@.blake3[],boolean,smallint,bigint,smallint)
    IS 'Exact direct overlap containment, optionally requiring every required ID. Union mode uses ordinal 0; per-part mode preserves duplicate request ordinals. floor is exclusive; kinds 1=claims, 2=observations, 3=both. cap=-1 returns complete sets. A nonnegative per-part cap classifies each kind separately: overflow returns one hub row (null carrier), never an arbitrary subset. All-key containment is laplace_containers.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_held_files(keys @extschema@.blake3[],
    trunk @extschema@.blake3 DEFAULT NULL, cap bigint DEFAULT -1, max_nodes bigint DEFAULT 400000,
    OUT file @extschema@.blake3, OUT keys bigint, OUT hubs bigint, OUT levels integer, OUT cut boolean,
    OUT unit @extschema@.blake3, OUT unit_length bigint, OUT counts bigint[], OUT unit_tier smallint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 256 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_held_files';
COMMENT ON FUNCTION @extschema@.laplace_held_files(@extschema@.blake3[],@extschema@.blake3,bigint,bigint)
    IS 'Up the container index from keys to the files that hold them, in one call: level zero is the capped laplace_holders nomination (a key past cap holders of a kind is a hub and is not walked), each later level one direct-container frontier, never through a file record. keys is the bit set of the keys (first 64) below the file. trunk keeps the files under that trunk. Each file has its unit (the keys'' lowest common ancestor in its content), the unit''s path length and tier, and each key''s occurrences below it. One row a file; none reached gives one row with a null file.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_standings(ids @extschema@.blake3[],
    OUT ordinal bigint, OUT claim @extschema@.blake3, OUT rating double precision,
    OUT deviation double precision, OUT volatility double precision, OUT matches bigint) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_standings';

CREATE OR REPLACE FUNCTION @extschema@.laplace_entities(ids @extschema@.blake3[],
    OUT ordinal bigint, OUT id @extschema@.blake3, OUT tier smallint,
    OUT coord @extschema:postgis@.geometry, OUT hilbert bigint, OUT mask bigint) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_entities';
CREATE OR REPLACE FUNCTION @extschema@.laplace_physicalities(ids @extschema@.blake3[],
    OUT ordinal bigint, OUT entity @extschema@.blake3, OUT tier smallint,
    OUT path @extschema:postgis@.geometry, OUT mask bigint) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_physicalities';

CREATE OR REPLACE FUNCTION @extschema@.laplace_constituents(ids @extschema@.blake3[],
    OUT entity @extschema@.blake3, OUT ordinal integer, OUT child @extschema@.blake3,
    OUT run_length integer, OUT flags bigint) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_constituents';

-- Walks over the stored DAG and the rated claims: each one Native frontier, one set read a level.
CREATE OR REPLACE FUNCTION @extschema@.laplace_precedes(@extschema:postgis@.geometry, @extschema@.blake3[]) RETURNS @extschema@.blake3[]
    AS 'MODULE_PATHNAME', 'laplace_precedes' LANGUAGE C IMMUTABLE STRICT COST 1000 PARALLEL SAFE;
COMMENT ON FUNCTION @extschema@.laplace_precedes(@extschema:postgis@.geometry, @extschema@.blake3[])
    IS 'Antecedents: the ID of the constituent before every run of the phrase inside the path (within a run its own constituent), the mirror of laplace_follows; NULL when nothing precedes. Packed vertices are compared as identities, never as geometry.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_below(ids @extschema@.blake3[], depth integer DEFAULT 1,
    OUT parent @extschema@.blake3, OUT ordinal integer, OUT child @extschema@.blake3,
    OUT run_length integer, OUT flags bigint, OUT depth integer) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 256 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_below';
COMMENT ON FUNCTION @extschema@.laplace_below(@extschema@.blake3[], integer)
    IS 'Downward closure of laplace_constituents: depth 1 is each input''s constituents (vertex ordinal, run and flags as laplace_constituents gives them), each later depth the constituents of the children not yet expanded. Every parent is expanded once; one path read a depth.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_above(ids @extschema@.blake3[], hops integer DEFAULT 1, cap bigint DEFAULT 1000,
    OUT entity @extschema@.blake3, OUT tier smallint, OUT hops integer) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 256 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_above';
COMMENT ON FUNCTION @extschema@.laplace_above(@extschema@.blake3[], integer, bigint)
    IS 'Upward containment closure: hop 1 every direct container of the IDs through the container index (no mask), each later hop the direct containers of the previous hop''s new entities. Every entity once, at the first hop it is met; inputs excluded. At most cap rows, in hop order then ID order; one container read a hop and one tier read a call.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_pull(seeds @extschema@.blake3[], goals @extschema@.blake3[] DEFAULT NULL,
    hops integer DEFAULT 4, fan bigint DEFAULT 64, nodes bigint DEFAULT 4096,
    middle @extschema@.blake3[] DEFAULT NULL, refuse @extschema@.blake3[] DEFAULT '{}',
    k double precision DEFAULT 2, per_hop double precision DEFAULT 0.05,
    OUT ordinal bigint, OUT id @extschema@.blake3, OUT seed @extschema@.blake3, OUT via @extschema@.blake3,
    OUT claim @extschema@.blake3, OUT relation @extschema@.blake3, OUT rating double precision,
    OUT deviation double precision, OUT cost double precision, OUT hops integer, OUT hub boolean,
    OUT goal boolean, OUT chain @extschema@.blake3[], OUT claims @extschema@.blake3[]) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 256 SUPPORT @extschema@.laplace_srf_support
    AS 'MODULE_PATHNAME', 'laplace_pull';
CREATE OR REPLACE FUNCTION @extschema@.laplace_pull(seeds @extschema@.blake3[], goals @extschema@.blake3[],
    hops integer, fan bigint, nodes bigint,
    middle @extschema@.blake3[], refuse @extschema@.blake3[],
    k double precision, per_hop double precision, witnesses @extschema@.blake3[],
    OUT ordinal bigint, OUT id @extschema@.blake3, OUT seed @extschema@.blake3, OUT via @extschema@.blake3,
    OUT claim @extschema@.blake3, OUT relation @extschema@.blake3, OUT rating double precision,
    OUT deviation double precision, OUT cost double precision, OUT hops integer, OUT hub boolean,
    OUT goal boolean, OUT chain @extschema@.blake3[], OUT claims @extschema@.blake3[]) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 256 SUPPORT @extschema@.laplace_srf_support
    AS 'MODULE_PATHNAME', 'laplace_pull';
COMMENT ON FUNCTION @extschema@.laplace_pull(@extschema@.blake3[], @extschema@.blake3[], integer, bigint, bigint, @extschema@.blake3[], @extschema@.blake3[], double precision, double precision)
    IS 'Best-first search over rated claims (Native lp_frontier, crossing cost lp_cost(rating, k, per_hop)), the law of the Engine''s pull: seeds open at cost 0; open entities of least cost close in batches; each closed short of hops is read through its claims of three constituents (laplace_claims_each, either end, middle qualified by middle, refusals out) at most fan an entity, crossing from one end to the other. An entity holding more than fan claims is a hub, reached and not crossed; a seed is crossed through its fan lowest-ID claims. Stops when nothing is open, nodes entities are closed, or every goal is closed. One row a closed entity in close order with its seed, the entity and claim it was reached through, that claim''s middle and standing, the chain cost, hops, and the chain of entities and claims from its seed.';
CREATE OR REPLACE FUNCTION @extschema@.laplace_chains(roots @extschema@.blake3[], steps @extschema@.blake3[],
    programs integer[], depths integer[], inverse boolean[], cap bigint DEFAULT 4096,
    OUT root integer, OUT program integer, OUT path bigint, OUT depth integer, OUT claim @extschema@.blake3,
    OUT entity @extschema@.blake3, OUT relation @extschema@.blake3, OUT rating double precision,
    OUT deviation double precision, OUT matches bigint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 256 SUPPORT @extschema@.laplace_srf_support
    AS 'MODULE_PATHNAME', 'laplace_chains';
COMMENT ON FUNCTION @extschema@.laplace_chains(@extschema@.blake3[], @extschema@.blake3[], integer[], integer[], boolean[], bigint)
    IS 'Declared paths over rated claims: every root walks every program, one claim of three constituents a depth. Step i allows middle steps[i] at 0-based depth depths[i] of 1-based program programs[i], read from the subject (the path starts with the node) or, inverse, from the object. One set read a depth for the whole frontier; only complete paths are returned, one row a step with a per-call path index. A frontier or a depth''s reading larger than cap is an error and returns nothing partial.';

-- Stage admission, series accumulation, pending-list maintenance and merge live in api.sql (Native C).

CREATE OR REPLACE FUNCTION @extschema@.laplace_ensure_stage() RETURNS void
    LANGUAGE plpgsql
    AS $$
BEGIN
  IF to_regclass('stage.entity') IS NOT NULL
     AND to_regclass('stage.physicality') IS NOT NULL
     AND to_regclass('stage.attestation') IS NOT NULL
     AND EXISTS (SELECT FROM pg_attribute WHERE attrelid = to_regclass('stage.attestation') AND attname = 'tokens' AND NOT attisdropped) THEN RETURN; END IF;
  PERFORM pg_advisory_xact_lock(1279348803, -1);
  CREATE SCHEMA IF NOT EXISTS stage;
  CREATE UNLOGGED TABLE IF NOT EXISTS stage.entity (LIKE @extschema@.entity INCLUDING DEFAULTS);
  CREATE UNLOGGED TABLE IF NOT EXISTS stage.physicality (LIKE @extschema@.physicality INCLUDING DEFAULTS);
  CREATE UNLOGGED TABLE IF NOT EXISTS stage.attestation (
    claim @extschema@.blake3 NOT NULL, witness @extschema@.blake3 NOT NULL, lineage @extschema@.blake3,
    games bigint NOT NULL CHECK (games > 0),
    tokens bigint NOT NULL DEFAULT 1 CHECK (tokens >= games),
    score double precision NOT NULL CHECK (score BETWEEN 0 AND 1),
    "position" integer,
    trust double precision NOT NULL CHECK (trust BETWEEN -1 AND 1)
  );
  ALTER TABLE stage.attestation ADD COLUMN IF NOT EXISTS tokens bigint NOT NULL DEFAULT 1 CHECK (tokens >= games);
  ALTER TABLE stage.attestation ADD COLUMN IF NOT EXISTS "position" integer;
END $$;

CREATE OR REPLACE FUNCTION @extschema@.laplace_stage_partitions(paths regclass) RETURNS oid[]
    AS 'MODULE_PATHNAME','laplace_stage_partitions' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION @extschema@.laplace_clean_partitions(partitions oid[]) RETURNS bigint
    AS 'MODULE_PATHNAME','laplace_clean_partitions' LANGUAGE C STRICT;

-- laplace_merge_stage and laplace_recoordinate live in api.sql (Native C).

CREATE OR REPLACE FUNCTION @extschema@.laplace_schema_indexes() RETURNS void
    LANGUAGE plpgsql
    AS $$
BEGIN
  -- Distinct metrics retain their own exact access paths. Tier is a key prefix,
  -- so a sparse high-tier Hilbert search cannot walk the Unicode floor first.
  DROP INDEX IF EXISTS @extschema@.entity_hilbert;
  DROP INDEX IF EXISTS @extschema@.entity_angular;
  CREATE INDEX IF NOT EXISTS entity_tier_hilbert ON @extschema@.entity (tier, hilbert, id);
  CREATE INDEX IF NOT EXISTS entity_coord ON @extschema@.entity USING gist (coord @extschema@.laplace_point4d_ops);
  -- 32 MB a partition: a search scans the pending list, so it spills around a batch instead of holding a source.
  -- 4 MB spilled every few thousand paths (full-page images). 256 MB held the list until the source ended.
  CREATE INDEX IF NOT EXISTS physicality_paths ON @extschema@.physicality USING gin (path @extschema@.laplace_path_ops, mask @extschema@.laplace_mask_ops) WITH (gin_pending_list_limit = 32768);
  -- Assertion lookup has a different population from ordinary content lookup.
  -- PostgreSQL owns this canonical index's visibility, updates, WAL and rebuild.
  CREATE INDEX IF NOT EXISTS physicality_assertions ON @extschema@.physicality USING gin (path @extschema@.laplace_path_ops)
    WITH (gin_pending_list_limit = 32768) WHERE @extschema@.laplace_path_has_roles(path,6::smallint);
  -- Witness metadata (declared trust and optional lineage) lives on one canonical
  -- self-registration row per witness (claim = witness, games = 0). This partial
  -- unique index indexes only those registration rows (~16 KB) instead of every
  -- (claim, witness) testimony row.
  DROP INDEX IF EXISTS @extschema@.attestation_witness_lineage;
  DROP INDEX IF EXISTS @extschema@.attestation_witness;
  CREATE UNIQUE INDEX IF NOT EXISTS attestation_witness_reg
    ON @extschema@.attestation (claim) INCLUDE (lineage, trust)
    WHERE claim OPERATOR(@extschema@.=) witness;
  CREATE INDEX IF NOT EXISTS consensus_standing ON @extschema@.consensus
    (@extschema@.laplace_confidence(rating, deviation, 2::double precision) DESC, matches DESC, claim)
    WHERE matches > 0;
  IF EXISTS (SELECT 1 FROM pg_catalog.pg_roles WHERE rolname = 'laplace_modular_web') THEN
    EXECUTE pg_catalog.format('GRANT CONNECT ON DATABASE %I TO laplace_modular_web', pg_catalog.current_database());
    GRANT USAGE ON SCHEMA @extschema@ TO laplace_modular_web;
    GRANT SELECT ON ALL TABLES IN SCHEMA @extschema@ TO laplace_modular_web;
    ALTER DEFAULT PRIVILEGES IN SCHEMA @extschema@ GRANT SELECT ON TABLES TO laplace_modular_web;
  END IF;
END $$;
SELECT @extschema@.laplace_schema_indexes();

CREATE OR REPLACE FUNCTION @extschema@.laplace_restore_function_bindings() RETURNS void
AS 'MODULE_PATHNAME','laplace_restore_function_bindings' LANGUAGE C VOLATILE PARALLEL UNSAFE;

-- Identity-space reference preparation shared by every API adapter.
CREATE OR REPLACE FUNCTION @extschema@.laplace_resolve_refs(ids bytea[],labels text[],OUT ordinal bigint,OUT id bytea,OUT label text,OUT present boolean)
RETURNS SETOF record AS 'MODULE_PATHNAME','laplace_resolve_refs' LANGUAGE C STABLE SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE;
CREATE OR REPLACE FUNCTION @extschema@.laplace_resolve_ref(reference text) RETURNS bytea
AS 'MODULE_PATHNAME','laplace_resolve_ref' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION @extschema@.laplace_claim_preview(ids @extschema@.blake3[],OUT i bigint,OUT entity @extschema@.blake3,
 OUT predicate @extschema@.blake3,OUT object @extschema@.blake3,OUT type_label text,OUT fact text)
RETURNS SETOF record AS 'MODULE_PATHNAME','laplace_claim_preview' LANGUAGE C STABLE STRICT SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE;

-- Ordered structure is projected from the canonical registered DAG at every tier.
CREATE OR REPLACE FUNCTION @extschema@.laplace_separator_ids() RETURNS @extschema@.blake3[]
AS 'MODULE_PATHNAME','laplace_separator_ids' LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE OR REPLACE FUNCTION @extschema@.laplace_ordered_pair_counts(vocab @extschema@.blake3[],lo integer,hi integer,
 roots @extschema@.blake3[] DEFAULT NULL,separators @extschema@.blake3[] DEFAULT @extschema@.laplace_separator_ids(),
 OUT container @extschema@.blake3,OUT gap integer,OUT subject @extschema@.blake3,OUT object @extschema@.blake3,OUT occurrences bigint)
RETURNS SETOF record AS 'MODULE_PATHNAME','laplace_ordered_pair_counts' LANGUAGE C STABLE SUPPORT @extschema@.laplace_srf_support PARALLEL RESTRICTED;

-- Recover missing operational measurements from stored testimony. This never
-- registers another witness, changes a claim, or runs the standing solver.
CREATE OR REPLACE PROCEDURE @extschema@.laplace_measure_attestations(roots @extschema@.blake3[] DEFAULT NULL)
LANGUAGE plpgsql AS $$
DECLARE selected record;
BEGIN
  FOR selected IN
    SELECT witness AS id FROM @extschema@.laplace_witness_trust(roots)
  LOOP
    PERFORM @extschema@.laplace_measure_witness(selected.id);
    COMMIT;
  END LOOP;
END $$;
