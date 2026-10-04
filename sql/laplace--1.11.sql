-- Laplace-postgres 1.3: Laplace's 4D expansion of PostgreSQL and PostGIS.
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

-- The 4D index on real coordinates (laplace_point4d_ops): GiST over geometry POINT ZM with double-precision keys,
-- ordered by <~>, the exact 4D Euclidean distance (laplace_distance4d). ORDER BY coord <~> q LIMIT k reads the index
-- in exact distance order; no row is rechecked. PostGIS's gist_geometry_ops_nd keys are float32 boxes and its <<->>
-- orders by their centres.
CREATE TYPE laplace_box4d;
CREATE FUNCTION laplace_box4d_in(cstring) RETURNS laplace_box4d AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_box4d_out(laplace_box4d) RETURNS cstring AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE TYPE laplace_box4d (INPUT = laplace_box4d_in, OUTPUT = laplace_box4d_out,
                           INTERNALLENGTH = 64, ALIGNMENT = double, STORAGE = plain);
CREATE FUNCTION laplace_point4d_distance(geometry, geometry) RETURNS double precision
  AS 'MODULE_PATHNAME', 'laplace_point4d_distance' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR <~> (LEFTARG = geometry, RIGHTARG = geometry, FUNCTION = laplace_point4d_distance, COMMUTATOR = <~>);
CREATE FUNCTION laplace_point4d_gist_consistent(internal, geometry, smallint, oid, internal) RETURNS boolean
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_point4d_gist_union(internal, internal) RETURNS laplace_box4d
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_point4d_gist_compress(internal) RETURNS internal
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_point4d_gist_penalty(internal, internal, internal) RETURNS internal
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_point4d_gist_picksplit(internal, internal) RETURNS internal
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_point4d_gist_same(laplace_box4d, laplace_box4d, internal) RETURNS internal
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_point4d_gist_distance(internal, geometry, smallint, oid, internal) RETURNS double precision
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_point4d_gist_sortsupport(internal) RETURNS void
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR CLASS laplace_point4d_ops FOR TYPE geometry USING gist AS
  OPERATOR 15 <~> (geometry, geometry) FOR ORDER BY float_ops,
  FUNCTION 1 laplace_point4d_gist_consistent(internal, geometry, smallint, oid, internal),
  FUNCTION 2 laplace_point4d_gist_union(internal, internal),
  FUNCTION 3 laplace_point4d_gist_compress(internal),
  FUNCTION 5 laplace_point4d_gist_penalty(internal, internal, internal),
  FUNCTION 6 laplace_point4d_gist_picksplit(internal, internal),
  FUNCTION 7 laplace_point4d_gist_same(laplace_box4d, laplace_box4d, internal),
  FUNCTION 8 laplace_point4d_gist_distance(internal, geometry, smallint, oid, internal),
  FUNCTION 11 laplace_point4d_gist_sortsupport(internal),
  STORAGE laplace_box4d;
-- Angles on S^3 (laplace_angular4d_ops): <=> is the angle in radians between two points' directions, the point over
-- its length, laplace_direction4d. The class keys each point's direction, so ORDER BY coord <=> q LIMIT k reads the
-- index in exact angle order on the coordinate column itself; no row is rechecked. The origin has no direction (NaN).
-- The angle is 2 asin(chord/2), monotone in the chord as the index needs; it is within an ulp or two of the true
-- angle (a right angle comes out one ulp above pi/2, from the rounded chord).
CREATE FUNCTION laplace_angular4d(geometry, geometry) RETURNS double precision
  AS 'MODULE_PATHNAME', 'laplace_angular4d' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR <=> (LEFTARG = geometry, RIGHTARG = geometry, FUNCTION = laplace_angular4d, COMMUTATOR = <=>);
CREATE FUNCTION laplace_direction4d_ewkb(geometry) RETURNS bytea
  AS 'MODULE_PATHNAME', 'laplace_direction4d_ewkb' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_direction4d(geometry) RETURNS geometry
  AS $$ SELECT ST_GeomFromEWKB(laplace_direction4d_ewkb($1)) $$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_angular4d_gist_compress(internal) RETURNS internal
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_angular4d_gist_distance(internal, geometry, smallint, oid, internal) RETURNS double precision
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR CLASS laplace_angular4d_ops FOR TYPE geometry USING gist AS
  OPERATOR 15 <=> (geometry, geometry) FOR ORDER BY float_ops,
  FUNCTION 1 laplace_point4d_gist_consistent(internal, geometry, smallint, oid, internal),
  FUNCTION 2 laplace_point4d_gist_union(internal, internal),
  FUNCTION 3 laplace_angular4d_gist_compress(internal),
  FUNCTION 5 laplace_point4d_gist_penalty(internal, internal, internal),
  FUNCTION 6 laplace_point4d_gist_picksplit(internal, internal),
  FUNCTION 7 laplace_point4d_gist_same(laplace_box4d, laplace_box4d, internal),
  FUNCTION 8 laplace_angular4d_gist_distance(internal, geometry, smallint, oid, internal),
  FUNCTION 11 laplace_point4d_gist_sortsupport(internal),
  STORAGE laplace_box4d;
-- Paths by shape (laplace_path4d_ops): GiST over LINESTRING ZM ordered by <%>, the discrete Fréchet distance
-- (laplace_frechet4d). Its keys hold each path's box and its first and last vertex; the index gives a lower bound
-- (the query's vertices to the box, first to first, last to last) and the rows are rechecked, so ORDER BY path <%> q
-- LIMIT k comes out in exact Fréchet order.
CREATE OPERATOR <%> (LEFTARG = geometry, RIGHTARG = geometry, FUNCTION = laplace_frechet4d, COMMUTATOR = <%>);
CREATE TYPE laplace_path4d_key;
CREATE FUNCTION laplace_path4d_key_in(cstring) RETURNS laplace_path4d_key AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_path4d_key_out(laplace_path4d_key) RETURNS cstring AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE TYPE laplace_path4d_key (INPUT = laplace_path4d_key_in, OUTPUT = laplace_path4d_key_out,
                                INTERNALLENGTH = 128, ALIGNMENT = double, STORAGE = plain);
CREATE FUNCTION laplace_path4d_gist_compress(internal) RETURNS internal
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_path4d_gist_union(internal, internal) RETURNS laplace_path4d_key
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_path4d_gist_picksplit(internal, internal) RETURNS internal
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_path4d_gist_same(laplace_path4d_key, laplace_path4d_key, internal) RETURNS internal
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_path4d_gist_distance(internal, geometry, smallint, oid, internal) RETURNS double precision
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR CLASS laplace_path4d_ops FOR TYPE geometry USING gist AS
  OPERATOR 15 <%> (geometry, geometry) FOR ORDER BY float_ops,
  FUNCTION 1 laplace_point4d_gist_consistent(internal, geometry, smallint, oid, internal),
  FUNCTION 2 laplace_path4d_gist_union(internal, internal),
  FUNCTION 3 laplace_path4d_gist_compress(internal),
  FUNCTION 5 laplace_point4d_gist_penalty(internal, internal, internal),
  FUNCTION 6 laplace_path4d_gist_picksplit(internal, internal),
  FUNCTION 7 laplace_path4d_gist_same(laplace_path4d_key, laplace_path4d_key, internal),
  FUNCTION 8 laplace_path4d_gist_distance(internal, geometry, smallint, oid, internal),
  FUNCTION 11 laplace_point4d_gist_sortsupport(internal),
  STORAGE laplace_path4d_key;
-- 4D boxes through the Hilbert order: laplace_hilbert_ranges(lo, hi, budget) covers the box between two corners with
-- at most budget ranges of the stored hilbert values (lp_hilbert4 XOR 2^63, bigint order), exact where the budget
-- allows; laplace_within4d is the exact test on the coordinates. A box query on entity:
--   SELECT e.id FROM laplace_hilbert_ranges(lo, hi) r JOIN entity e ON e.hilbert BETWEEN r.lo AND r.hi
--   WHERE laplace_within4d(e.coord, lo, hi)
CREATE FUNCTION laplace_hilbert_ranges_flat(geometry, geometry, integer) RETURNS bigint[]
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_hilbert_ranges(lo geometry, hi geometry, budget integer DEFAULT 64)
  RETURNS TABLE (lo bigint, hi bigint)
  AS $$ SELECT f[2 * i - 1], f[2 * i] FROM (SELECT laplace_hilbert_ranges_flat($1, $2, $3) AS f) s,
              generate_series(1, coalesce(array_length(f, 1), 0) / 2) i $$
  LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE ROWS 64;
CREATE FUNCTION laplace_within4d(geometry, geometry, geometry) RETURNS boolean
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

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

-- The highway, in place: the types the resources list, and the mappings between them (laplace.highway names the
-- perf-cache, generated by laplace highway). A type is a slot of a list; its content's ID is its record's.
CREATE FUNCTION laplace_type(list text, value text) RETURNS integer
  AS 'MODULE_PATHNAME', 'laplace_type' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_type_key(list text, key text) RETURNS integer
  AS 'MODULE_PATHNAME', 'laplace_type_key' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_type_id(list text, slot integer) RETURNS blake3
  AS 'MODULE_PATHNAME', 'laplace_type_id' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_type_edges(a text, slot integer, b text) RETURNS integer[]
  AS 'MODULE_PATHNAME', 'laplace_type_edges' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_highway_fingerprint() RETURNS text
  AS 'MODULE_PATHNAME', 'laplace_highway_fingerprint' LANGUAGE C STABLE PARALLEL SAFE;

-- ------------------------------------------------------------------------------------------------ the mask
-- A row's mask (Semantics: Claims, Masks): 256 bits, what the row is (bits 0 to 7: a claim, a record, a tuple, a
-- file) and the types it holds, by the highway's layout. GIN keys are the positions of its set bits.
CREATE FUNCTION laplace_mask_has(bit, smallint) RETURNS boolean AS 'MODULE_PATHNAME', 'laplace_mask_has' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_has_all(bit, smallint[]) RETURNS boolean AS 'MODULE_PATHNAME', 'laplace_mask_has_all' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_has_any(bit, smallint[]) RETURNS boolean AS 'MODULE_PATHNAME', 'laplace_mask_has_any' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR ? (LEFTARG = bit, RIGHTARG = smallint, FUNCTION = laplace_mask_has, RESTRICT = contsel, JOIN = contjoinsel);
CREATE OPERATOR ?& (LEFTARG = bit, RIGHTARG = smallint[], FUNCTION = laplace_mask_has_all, RESTRICT = contsel, JOIN = contjoinsel);
CREATE OPERATOR ?| (LEFTARG = bit, RIGHTARG = smallint[], FUNCTION = laplace_mask_has_any, RESTRICT = contsel, JOIN = contjoinsel);
CREATE FUNCTION laplace_mask_extract_value(bit, internal, internal) RETURNS internal AS 'MODULE_PATHNAME', 'laplace_mask_extract_value' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_extract_query(internal, internal, int2, internal, internal, internal, internal) RETURNS internal AS 'MODULE_PATHNAME', 'laplace_mask_extract_query' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_consistent(internal, int2, internal, int4, internal, internal, internal, internal) RETURNS boolean AS 'MODULE_PATHNAME', 'laplace_mask_consistent' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_triconsistent(internal, int2, internal, int4, internal, internal, internal) RETURNS "char" AS 'MODULE_PATHNAME', 'laplace_mask_triconsistent' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR CLASS laplace_mask_ops FOR TYPE bit USING gin AS
  OPERATOR 1 ? (bit, smallint),
  OPERATOR 2 ?& (bit, smallint[]),
  OPERATOR 3 ?| (bit, smallint[]),
  FUNCTION 1 btint2cmp(smallint, smallint),
  FUNCTION 2 laplace_mask_extract_value(bit, internal, internal),
  FUNCTION 3 laplace_mask_extract_query(internal, internal, int2, internal, internal, internal, internal),
  FUNCTION 4 laplace_mask_consistent(internal, int2, internal, int4, internal, internal, internal, internal),
  FUNCTION 6 laplace_mask_triconsistent(internal, int2, internal, int4, internal, internal, internal),
  STORAGE smallint;
-- The mask bit of a type, from its content or its ID: the highway's field for its list, plus its slot; -1 when none.
-- The banks (manifest/banks.tsv): one mask per semantic group, on the row its group describes. A value's bit in its bank
-- is its frozen slot; laplace_bank_bit('kind', 'claim') is a row's kind, laplace_bank_bit('upos', 'NOUN') a part of speech.
CREATE FUNCTION laplace_bank_bit(bank text, value text) RETURNS smallint AS 'MODULE_PATHNAME', 'laplace_bank_bit' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_bank_of(blake3, OUT bank text, OUT grp text, OUT carrier text, OUT "bit" smallint) RETURNS record AS 'MODULE_PATHNAME', 'laplace_bank_of' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- ------------------------------------------------------------------------------------------------ the tables
-- The five tables, the extension's own (Storage: Physicality; Semantics), so a database is a Laplace database by
-- CREATE EXTENSION laplace, and pg_dump carries their data. An ID is a blake3 of pure content, and an entity is its
-- ID: one row for one ID, whatever tier or role its content is met in ("tier is altitude, not identity"). Its tier is
-- the lowest it is composed at: a column, never part of a key. So entities and physicalities partition by the ID's
-- hash alone, 256 ways by its first byte (an ID is a hash, so the parts fill evenly and a lookup by ID prunes to one),
-- the same ranges for both: an entity and its physicality are in partitions of one name. The keys are the IDs, and a
-- physicality's entity is a foreign key to its entity. PostgreSQL enforces a key on a partitioned table only when the
-- key holds every partition column, so the ID alone is what the partitions go by. Hilbert values are unsigned 64-bit
-- stored with the top bit flipped, so bigint order is Hilbert order.
CREATE TABLE IF NOT EXISTS entity (
  id       blake3   NOT NULL,
  tier     smallint NOT NULL,
  coord    geometry(PointZM) NOT NULL,           -- the real 4D coordinate; M is W
  hilbert  bigint   NOT NULL,
  PRIMARY KEY (id)
) PARTITION BY RANGE (id);
CREATE TABLE IF NOT EXISTS physicality (
  entity   blake3   NOT NULL REFERENCES entity (id),
  tier     smallint NOT NULL,
  hilbert  bigint   NOT NULL,
  path     geometry NOT NULL,                    -- the constituents' IDs in X/Y/Z, each vertex's M its run and what it is
  mask     bit(256) NOT NULL DEFAULT B'0'::bit(256),  -- what the row is, and the types it holds (laplace_mask_ops)
  PRIMARY KEY (entity)
) PARTITION BY RANGE (entity);
DO $$
DECLARE k int; lo text; hi text; r record;
BEGIN
  FOR k IN 0..255 LOOP
    lo := CASE WHEN k = 0 THEN 'MINVALUE' ELSE quote_literal(lpad(to_hex(k), 2, '0') || repeat('0', 30)) END;
    hi := CASE WHEN k = 255 THEN 'MAXVALUE' ELSE quote_literal(lpad(to_hex(k + 1), 2, '0') || repeat('0', 30)) END;
    EXECUTE format('CREATE TABLE IF NOT EXISTS entity_%s PARTITION OF entity FOR VALUES FROM (%s) TO (%s)', lpad(to_hex(k), 2, '0'), lo, hi);
    EXECUTE format('CREATE TABLE IF NOT EXISTS physicality_%s PARTITION OF physicality FOR VALUES FROM (%s) TO (%s)', lpad(to_hex(k), 2, '0'), lo, hi);
  END LOOP;
  -- a path's X/Y/Z are packed IDs, not positions: no statistics on them; container lookups take a millisecond, so no parallel workers per partition
  ALTER TABLE physicality ALTER COLUMN path SET STATISTICS 0;
  FOR r IN SELECT c.relname FROM pg_class c JOIN pg_inherits i ON i.inhrelid = c.oid WHERE c.relname ~ '^physicality_[0-9a-f]{2}$' AND c.relkind = 'r' LOOP
    EXECUTE format('ALTER TABLE %I SET (parallel_workers = 0)', r.relname);
  END LOOP;
END $$;
CREATE TABLE IF NOT EXISTS witness (
  id       blake3 PRIMARY KEY,                     -- an entity: whatever testifies, named as content
  lineage  blake3,                                 -- the witness it derives from, so copies are not independent
  trust    double precision NOT NULL             -- -1 .. 1: MANDATE is 1, no information 0, reliably wrong -1
);
-- Attestations and standings are partitioned as the content is: by the claim's ID hash, sixteen ways by its first
-- hex digit (Storage: Physicality, "Partitions go by the ID hash"; Sequence: Database 8.10, "entities, paths, and
-- statistics"). A lookup or an update by claim prunes to one partition; attestations append evenly across sixteen.
CREATE TABLE IF NOT EXISTS attestation (         -- a witness pulling on a strand: one row per (claim, witness)
  claim    blake3 NOT NULL,
  witness  blake3 NOT NULL,
  score    real NOT NULL,                        -- the series' score: win 1, draw 0.5, loss 0, or a score between, the mean over its games
  position integer,                              -- the place a witness gave the claim among its like, as given
  games    integer NOT NULL DEFAULT 1,           -- how many times this witness attested it: a claim is a game series, games plus a score
  PRIMARY KEY (claim, witness)                   -- one series for a claim and a witness
) PARTITION BY RANGE (claim);
CREATE TABLE IF NOT EXISTS consensus (           -- updated in place as matchups are played
  claim       blake3 NOT NULL,
  rating      double precision NOT NULL,
  deviation   double precision NOT NULL,
  volatility  double precision NOT NULL,
  matches     integer NOT NULL,
  PRIMARY KEY (claim)
) PARTITION BY RANGE (claim);
DO $$
DECLARE k int; lo text; hi text;
BEGIN
  FOR k IN 0..15 LOOP
    lo := CASE WHEN k = 0 THEN 'MINVALUE' ELSE quote_literal(to_hex(k) || repeat('0', 31)) END;
    hi := CASE WHEN k = 15 THEN 'MAXVALUE' ELSE quote_literal(to_hex(k + 1) || repeat('0', 31)) END;
    EXECUTE format('CREATE TABLE IF NOT EXISTS attestation_%s PARTITION OF attestation FOR VALUES FROM (%s) TO (%s)', to_hex(k), lo, hi);
    EXECUTE format('CREATE TABLE IF NOT EXISTS consensus_%s PARTITION OF consensus FOR VALUES FROM (%s) TO (%s) WITH (fillfactor = 80)', to_hex(k), lo, hi);
  END LOOP;
END $$;
-- the tables' data goes with a dump (tables a database had before this version are made the extension's by laplace deploy)
DO $$
DECLARE r record;
BEGIN
  FOR r IN SELECT c.oid::regclass AS t FROM pg_class c WHERE c.relkind = 'r' AND (c.relname ~ '^(entity|physicality)_[0-9a-f]{2}$' OR c.relname ~ '^(attestation|consensus)_[0-9a-f]$' OR c.relname = 'witness') LOOP
    PERFORM pg_catalog.pg_extension_config_dump(r.t, '');
  END LOOP;
END $$;
-- The indexes, all of them from the start; laplace index makes them again if one was dropped. Each is created on
-- the partitioned parent, which builds one per partition. The IDs are the primary keys (an entity's, its
-- physicality's, an attestation's claim and witness, a standing's claim): deduplication and attestations look up by
-- them. Besides: the Hilbert order, the real coordinates (GiST, 4D), the containers and the mask together (GIN),
-- attestations by witness.
CREATE FUNCTION laplace_schema_indexes() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  CREATE INDEX IF NOT EXISTS entity_hilbert ON entity (hilbert);
  CREATE INDEX IF NOT EXISTS entity_coord ON entity USING gist (coord laplace_point4d_ops);
  -- 32 MB a partition: a search scans the pending list, so it spills around a batch instead of holding a source.
  -- 4 MB spilled every few thousand paths (full-page images). 256 MB held the list until the source ended.
  CREATE INDEX IF NOT EXISTS physicality_paths ON physicality USING gin (path laplace_path_ops, mask laplace_mask_ops) WITH (gin_pending_list_limit = 32768);
  CREATE INDEX IF NOT EXISTS attestation_witness ON attestation (witness);
END $$;
DROP INDEX IF EXISTS physicality_paths;       -- the container index without the mask, where a database had it
SELECT laplace_schema_indexes();

-- ------------------------------------------------------------------------------------------------ the web
-- The web's reads take, besides the parts, the mask bits a row must have: laplace_bank_bit('kind', 'claim') for claims,
-- a type's bit for claims of that type; an empty array asks for any row.
DROP FUNCTION laplace_claims(blake3[], bigint);
DROP FUNCTION laplace_claims_each(blake3[], bigint);
DROP FUNCTION laplace_containers(blake3[]);
CREATE FUNCTION laplace_claims(parts blake3[], fan bigint, bits smallint[], OUT entity blake3, OUT path geometry, OUT rating double precision, OUT deviation double precision, OUT volatility double precision, OUT matches integer) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_claims' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 64;
CREATE FUNCTION laplace_claims_each(ids blake3[], fan bigint, bits smallint[], OUT i bigint, OUT entity blake3, OUT path geometry, OUT rating double precision, OUT deviation double precision, OUT volatility double precision, OUT matches integer) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_claims_each' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 1024;
CREATE FUNCTION laplace_containers(parts blake3[], bits smallint[], OUT entity blake3, OUT path geometry, OUT tier smallint, OUT mask bit) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_containers' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 256;

CREATE FUNCTION laplace_forward(ids blake3[], fan bigint, OUT i integer, OUT j integer, OUT paths bigint, OUT runs bigint, OUT next blake3, OUT times bigint)
RETURNS SETOF record AS 'MODULE_PATHNAME', 'laplace_forward' LANGUAGE C STABLE STRICT PARALLEL SAFE;
-- A restriction is applied before the sort (Sequence 15.5): the claim reads take, besides the parts, the fan and the
-- mask bits, the predicates the pass's firmware refuses. A claim holding one of them between its first part and its
-- last is taken out in the read itself, so a refused strand never takes a place the fan leaves; '{}' refuses nothing.
CREATE FUNCTION laplace_middle_any(geometry, blake3[]) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_middle_any' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
DROP FUNCTION laplace_claims(blake3[], bigint, smallint[]);
DROP FUNCTION laplace_claims_each(blake3[], bigint, smallint[]);
CREATE FUNCTION laplace_claims(parts blake3[], fan bigint, bits smallint[], refuse blake3[], OUT entity blake3, OUT path geometry, OUT rating double precision, OUT deviation double precision, OUT volatility double precision, OUT matches integer) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_claims' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 64;
CREATE FUNCTION laplace_claims_each(ids blake3[], fan bigint, bits smallint[], refuse blake3[], OUT i bigint, OUT entity blake3, OUT path geometry, OUT rating double precision, OUT deviation double precision, OUT volatility double precision, OUT matches integer) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_claims_each' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 1024;
-- COUPLE as one coarse native operator (Sequence 20.2; spec 36, native execution grain): the strands, the containment
-- and the shape of an admitted observation in one call, set access through SPI and everything else native. shape:
-- -1 none, 0 Fréchet, 1 Fréchet with shape_n outliers, 2 DTW, 3 EDR within shape_n; keep: how many nearest curves.
CREATE FUNCTION laplace_couple(occ blake3[], fan bigint, refuse blake3[], shape smallint, shape_n double precision, keep integer,
  OUT entity blake3, OUT occ integer, OUT route smallint, OUT rating double precision, OUT deviation double precision, OUT volatility double precision,
  OUT via blake3, OUT rel blake3, OUT tier smallint, OUT distance double precision, OUT vertices blake3[]) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_couple' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 4096;
