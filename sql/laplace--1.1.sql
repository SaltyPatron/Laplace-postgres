-- Laplace-postgres 1.0: Laplace's 4D expansion of PostgreSQL and PostGIS.
\echo Use "CREATE EXTENSION laplace" to load this file. \quit

-- The ID of everything: a BLAKE3 hash, 128 bits. 16 fixed bytes, written as 32 hexadecimal digits, ordered and
-- compared as bytes. Every bit of it is the hash.
CREATE TYPE blake3;
CREATE FUNCTION blake3_in(cstring) RETURNS blake3 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION blake3_out(blake3) RETURNS cstring AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION blake3_recv(internal) RETURNS blake3 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION blake3_send(blake3) RETURNS bytea AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE TYPE blake3 (INPUT = blake3_in, OUTPUT = blake3_out, RECEIVE = blake3_recv, SEND = blake3_send,
                    INTERNALLENGTH = 16, ALIGNMENT = char, STORAGE = plain);
CREATE FUNCTION blake3_eq(blake3, blake3) RETURNS boolean AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION blake3_ne(blake3, blake3) RETURNS boolean AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION blake3_lt(blake3, blake3) RETURNS boolean AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION blake3_le(blake3, blake3) RETURNS boolean AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION blake3_gt(blake3, blake3) RETURNS boolean AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION blake3_ge(blake3, blake3) RETURNS boolean AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION blake3_cmp(blake3, blake3) RETURNS integer AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION blake3_hash(blake3) RETURNS integer AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION blake3_hash_extended(blake3, bigint) RETURNS bigint AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR =  (LEFTARG = blake3, RIGHTARG = blake3, FUNCTION = blake3_eq, COMMUTATOR = =,  NEGATOR = <>, RESTRICT = eqsel, JOIN = eqjoinsel, HASHES, MERGES);
CREATE OPERATOR <> (LEFTARG = blake3, RIGHTARG = blake3, FUNCTION = blake3_ne, COMMUTATOR = <>, NEGATOR = =,  RESTRICT = neqsel, JOIN = neqjoinsel);
CREATE OPERATOR <  (LEFTARG = blake3, RIGHTARG = blake3, FUNCTION = blake3_lt, COMMUTATOR = >,  NEGATOR = >=, RESTRICT = scalarltsel, JOIN = scalarltjoinsel);
CREATE OPERATOR <= (LEFTARG = blake3, RIGHTARG = blake3, FUNCTION = blake3_le, COMMUTATOR = >=, NEGATOR = >,  RESTRICT = scalarlesel, JOIN = scalarlejoinsel);
CREATE OPERATOR >  (LEFTARG = blake3, RIGHTARG = blake3, FUNCTION = blake3_gt, COMMUTATOR = <,  NEGATOR = <=, RESTRICT = scalargtsel, JOIN = scalargtjoinsel);
CREATE OPERATOR >= (LEFTARG = blake3, RIGHTARG = blake3, FUNCTION = blake3_ge, COMMUTATOR = <=, NEGATOR = <,  RESTRICT = scalargesel, JOIN = scalargejoinsel);
CREATE OPERATOR CLASS blake3_ops DEFAULT FOR TYPE blake3 USING btree AS
  OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 blake3_cmp(blake3, blake3);
CREATE OPERATOR CLASS blake3_hash_ops DEFAULT FOR TYPE blake3 USING hash AS
  OPERATOR 1 =, FUNCTION 1 blake3_hash(blake3), FUNCTION 2 blake3_hash_extended(blake3, bigint);

-- Identity. Constant arguments fold at plan time, so a lookup by computed ID becomes an index lookup.
CREATE FUNCTION laplace_cp_id(integer) RETURNS blake3
  AS 'MODULE_PATHNAME', 'laplace_cp_id' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_text_id(text) RETURNS blake3
  AS 'MODULE_PATHNAME', 'laplace_text_id' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_compose(blake3[]) RETURNS blake3
  AS 'MODULE_PATHNAME', 'laplace_compose' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Physicality paths: children's IDs in the X/Y/Z mantissas, run lengths in M.
CREATE FUNCTION laplace_path_ewkb(blake3[]) RETURNS bytea
  AS 'MODULE_PATHNAME', 'laplace_path_ewkb' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_path(blake3[]) RETURNS geometry
  AS $$ SELECT ST_GeomFromEWKB(laplace_path_ewkb($1)) $$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;

-- The distinct IDs a path holds: the key of the GIN index that finds containers.
-- COST 10000 (measured): decoding a whole path is expensive for long paths (a book's trunk has thousands of vertices).
-- At lower costs the planner scans the partition of whole books sequentially, decoding every book on every lookup
-- (90 ms of a 111 ms query). The parallel workers the high cost would invite are prevented by the schema instead.
CREATE FUNCTION laplace_vertex_ids(geometry) RETURNS blake3[]
  AS 'MODULE_PATHNAME', 'laplace_vertex_ids' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10000;

-- Continuations: the ID after every run of the phrase inside the path.
CREATE FUNCTION laplace_follows(geometry, blake3[]) RETURNS blake3[]
  AS 'MODULE_PATHNAME', 'laplace_follows' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

-- 4D on real coordinates.
CREATE FUNCTION laplace_distance4d(geometry, geometry) RETURNS double precision
  AS 'MODULE_PATHNAME', 'laplace_distance4d' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_frechet4d(geometry, geometry) RETURNS double precision
  AS 'MODULE_PATHNAME', 'laplace_frechet4d' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_hilbert4(geometry) RETURNS bigint
  AS 'MODULE_PATHNAME', 'laplace_hilbert4' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_inside(geometry) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_inside' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION laplace_centroid4d_step(internal, geometry) RETURNS internal
  AS 'MODULE_PATHNAME', 'laplace_centroid4d_step' LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE FUNCTION laplace_centroid4d_final(internal) RETURNS bytea
  AS 'MODULE_PATHNAME', 'laplace_centroid4d_final' LANGUAGE C IMMUTABLE PARALLEL SAFE;
-- The exact 4D centroid of POINT ZM values, as EWKB (ST_GeomFromEWKB turns it into a geometry).
CREATE AGGREGATE laplace_centroid4d_ewkb(geometry) (
  SFUNC = laplace_centroid4d_step, STYPE = internal, FINALFUNC = laplace_centroid4d_final);

-- Coordinates from the tier-0 perf-cache (laplace.tier0), computed in place: a lookup by computed Hilbert key prunes
-- to the one partition that can hold the entity.
CREATE FUNCTION laplace_text_coord_ewkb(text) RETURNS bytea
  AS 'MODULE_PATHNAME', 'laplace_text_coord_ewkb' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_text_coord(text) RETURNS geometry
  AS $$ SELECT ST_GeomFromEWKB(laplace_text_coord_ewkb($1)) $$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_text_hilbert(text) RETURNS bigint
  AS 'MODULE_PATHNAME', 'laplace_text_hilbert' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_cp_coord_ewkb(integer) RETURNS bytea
  AS 'MODULE_PATHNAME', 'laplace_cp_coord_ewkb' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Recomposition: any entity back to its text, walking physicality paths down to tier 0.
CREATE FUNCTION laplace_text(blake3) RETURNS text
  AS 'MODULE_PATHNAME', 'laplace_text' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- Observability.
CREATE FUNCTION laplace_isa() RETURNS text
  AS 'MODULE_PATHNAME', 'laplace_isa' LANGUAGE C STABLE PARALLEL SAFE;

-- GIN over path geometry.

-- A path holds these IDs (all of them) / any of them: answered exactly from the index, with no recheck.
CREATE FUNCTION laplace_path_contains(geometry, blake3[]) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_path_contains' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_path_overlaps(geometry, blake3[]) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_path_overlaps' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR @> (LEFTARG = geometry, RIGHTARG = blake3[], FUNCTION = laplace_path_contains);
CREATE OPERATOR && (LEFTARG = geometry, RIGHTARG = blake3[], FUNCTION = laplace_path_overlaps);

CREATE FUNCTION laplace_gin_extract_value(geometry, internal, internal) RETURNS internal
  AS 'MODULE_PATHNAME', 'laplace_gin_extract_value' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_gin_extract_query(blake3[], internal, int2, internal, internal, internal, internal) RETURNS internal
  AS 'MODULE_PATHNAME', 'laplace_gin_extract_query' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_gin_consistent(internal, int2, blake3[], int4, internal, internal, internal, internal) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_gin_consistent' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_gin_triconsistent(internal, int2, blake3[], int4, internal, internal, internal) RETURNS "char"
  AS 'MODULE_PATHNAME', 'laplace_gin_triconsistent' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR CLASS laplace_path_ops FOR TYPE geometry USING gin AS
  OPERATOR 3 && (geometry, blake3[]),
  OPERATOR 7 @> (geometry, blake3[]),
  FUNCTION 1 blake3_cmp(blake3, blake3),
  FUNCTION 2 laplace_gin_extract_value(geometry, internal, internal),
  FUNCTION 3 laplace_gin_extract_query(blake3[], internal, int2, internal, internal, internal, internal),
  FUNCTION 4 laplace_gin_consistent(internal, int2, blake3[], int4, internal, internal, internal, internal),
  FUNCTION 6 laplace_gin_triconsistent(internal, int2, blake3[], int4, internal, internal, internal),
  STORAGE blake3;

-- selectivity for the path operators. A path holds few of all IDs, so the planner is told
-- containment and overlap are selective and reaches for the index instead of decoding whole partitions.
ALTER OPERATOR @> (geometry, blake3[]) SET (RESTRICT = contsel, JOIN = contjoinsel);
ALTER OPERATOR && (geometry, blake3[]) SET (RESTRICT = contsel, JOIN = contjoinsel);

-- the path operators decode a whole path when evaluated outside the index (a file's trunk
-- holds every paragraph), so they carry that cost and the planner uses the index even on small partitions.
ALTER FUNCTION laplace_path_contains(geometry, blake3[]) COST 10000;
ALTER FUNCTION laplace_path_overlaps(geometry, blake3[]) COST 10000;

-- how many times a path holds each of a set of IDs, runs included, so a caller walking
-- the DAG receives counts instead of whole paths.
CREATE FUNCTION laplace_path_times(geometry, blake3[], OUT id blake3, OUT times bigint) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_path_times' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000 ROWS 4;

-- a text's entity computed in place by the one decomposition of text (UAX #29), the shape
-- measures beside Fréchet, a consensus's confidence, and the fingerprint of the tier 0 in use.

-- The entity of a text: what the engine records for the same text. laplace_id('Sherlock Holmes') is the ID of
-- [[S,h,e,r,l,o,c,k], ' ', [H,o,l,m,e,s]]. (laplace_text_id composes a text's codepoints directly, as one word.)
CREATE FUNCTION laplace_id(text) RETURNS blake3
  AS 'MODULE_PATHNAME', 'laplace_id' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_tier(text) RETURNS smallint
  AS 'MODULE_PATHNAME', 'laplace_tier' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_coord_ewkb(text) RETURNS bytea
  AS 'MODULE_PATHNAME', 'laplace_coord_ewkb' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_coord(text) RETURNS geometry
  AS $$ SELECT ST_GeomFromEWKB(laplace_coord_ewkb($1)) $$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_hilbert(text) RETURNS bigint
  AS 'MODULE_PATHNAME', 'laplace_hilbert' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- Its constituents in order, repeats included: the phrase to look for inside paths (path @> laplace_parts('...')).
CREATE FUNCTION laplace_parts(text) RETURNS blake3[]
  AS 'MODULE_PATHNAME', 'laplace_parts' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Shape measures on real coordinates, each for its purpose.
CREATE FUNCTION laplace_frechet4d(geometry, geometry, integer) RETURNS double precision      -- up to k vertices of each skipped
  AS 'MODULE_PATHNAME', 'laplace_frechet4d_outliers' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_dtw4d(geometry, geometry) RETURNS double precision
  AS 'MODULE_PATHNAME', 'laplace_dtw4d' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_edr4d(geometry, geometry, double precision) RETURNS bigint           -- edits, vertices within eps equal
  AS 'MODULE_PATHNAME', 'laplace_edr4d' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- How hard a strand tugs back: a consensus's chance of beating the anchor, read k deviations below its rating.
CREATE FUNCTION laplace_confidence(rating double precision, deviation double precision, k double precision DEFAULT 2)
  RETURNS double precision AS 'MODULE_PATHNAME', 'laplace_confidence' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Which tier 0 this database computes with: two installs with one fingerprint give the same content the same coordinates.
CREATE FUNCTION laplace_fingerprint() RETURNS text
  AS 'MODULE_PATHNAME', 'laplace_fingerprint' LANGUAGE C STABLE PARALLEL SAFE;

-- the flags that go with tier 0. What the Unicode Standard says of a codepoint, read from
-- the memory-mapped flags by the standard's own names; no table is read.

-- The codepoint an atom is, or NULL for an entity that is not an atom.
CREATE FUNCTION laplace_codepoint(blake3) RETURNS integer
  AS 'MODULE_PATHNAME', 'laplace_codepoint' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- A codepoint's 256 bits.
CREATE FUNCTION laplace_flags(integer) RETURNS bytea
  AS 'MODULE_PATHNAME', 'laplace_cp_flags' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- What a property is, of a codepoint or of an atom: laplace_said(65, 'General_Category') is Uppercase_Letter.
CREATE FUNCTION laplace_said(integer, text) RETURNS text
  AS 'MODULE_PATHNAME', 'laplace_cp_said' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_said(blake3, text) RETURNS text
  AS 'MODULE_PATHNAME', 'laplace_said' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- Whether a property has a value: laplace_is(id, 'script', 'Greek'). Names are matched by the standard's rule.
CREATE FUNCTION laplace_is(integer, text, text) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_cp_is' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_is(blake3, text, text) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_is' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;


-- The web: what tugs back when a strand is pulled. Each plans its statement once per backend and keeps the plan; each
-- call is one executor run over a set. A claim's tier is one above its highest part, so the claims holding an entity
-- are read above the entity's tier only, through the container index on those partitions, joined to the consensus.
CREATE FUNCTION laplace_claims(parts blake3[], fan bigint, OUT entity blake3, OUT path geometry, OUT rating double precision, OUT deviation double precision, OUT volatility double precision, OUT matches integer) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_claims' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 64;
CREATE FUNCTION laplace_claims_each(ids blake3[], fan bigint, OUT i bigint, OUT entity blake3, OUT path geometry, OUT rating double precision, OUT deviation double precision, OUT volatility double precision, OUT matches integer) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_claims_each' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 1024;
CREATE FUNCTION laplace_containers(parts blake3[], OUT entity blake3, OUT path geometry, OUT tier smallint, OUT attested boolean) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_containers' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 256;
CREATE FUNCTION laplace_fills(keys blake3[], OUT entity blake3, OUT id blake3, OUT times bigint, OUT tier smallint) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_fills' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 256;
CREATE FUNCTION laplace_paths(ids blake3[], OUT entity blake3, OUT path geometry) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_paths' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 64;
CREATE FUNCTION laplace_attested(claims blake3[], OUT claim blake3, OUT witness blake3, OUT "position" integer, OUT trust double precision) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_attested' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 256;
