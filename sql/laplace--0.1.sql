-- Laplace-postgres 0.1: Laplace's 4D expansion of PostgreSQL and PostGIS.
\echo Use "CREATE EXTENSION laplace" to load this file. \quit

-- Identity. Constant arguments fold at plan time, so a lookup by computed ID becomes an index lookup.
CREATE FUNCTION laplace_cp_id(integer) RETURNS uuid
  AS 'MODULE_PATHNAME', 'laplace_cp_id' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_text_id(text) RETURNS uuid
  AS 'MODULE_PATHNAME', 'laplace_text_id' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_compose(uuid[]) RETURNS uuid
  AS 'MODULE_PATHNAME', 'laplace_compose' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Physicality paths: children's IDs in the X/Y/Z mantissas, run lengths in M.
CREATE FUNCTION laplace_path_ewkb(uuid[]) RETURNS bytea
  AS 'MODULE_PATHNAME', 'laplace_path_ewkb' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_path(uuid[]) RETURNS geometry
  AS $$ SELECT ST_GeomFromEWKB(laplace_path_ewkb($1)) $$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;

-- The distinct IDs a path holds: the key of the GIN index that finds containers.
-- COST 10000 (measured): decoding a whole path is expensive for long paths (a book's trunk has thousands of vertices).
-- At lower costs the planner scans the partition of whole books sequentially, decoding every book on every lookup
-- (90 ms of a 111 ms query). The parallel workers the high cost would invite are prevented by the schema instead.
CREATE FUNCTION laplace_vertex_ids(geometry) RETURNS uuid[]
  AS 'MODULE_PATHNAME', 'laplace_vertex_ids' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10000;

-- Continuations: the ID after every run of the phrase inside the path.
CREATE FUNCTION laplace_follows(geometry, uuid[]) RETURNS uuid[]
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
CREATE FUNCTION laplace_text(uuid) RETURNS text
  AS 'MODULE_PATHNAME', 'laplace_text' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- Observability.
CREATE FUNCTION laplace_isa() RETURNS text
  AS 'MODULE_PATHNAME', 'laplace_isa' LANGUAGE C STABLE PARALLEL SAFE;
