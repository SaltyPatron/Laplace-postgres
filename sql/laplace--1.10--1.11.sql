-- 1.11: the 4D index on real coordinates, laplace_point4d_ops (double-precision keys, exact <~> order), in place
-- of PostGIS's gist_geometry_ops_nd on entity.coord. The index is built again, sorted.
\echo Use "ALTER EXTENSION laplace UPDATE TO '1.11'" to load this file. \quit

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
-- <%%>: the discrete Hausdorff distance between the vertex sets (order ignored), ordered by the same class.
CREATE FUNCTION laplace_hausdorff4d(geometry, geometry) RETURNS double precision
  AS 'MODULE_PATHNAME', 'laplace_hausdorff4d' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR <%%> (LEFTARG = geometry, RIGHTARG = geometry, FUNCTION = laplace_hausdorff4d, COMMUTATOR = <%%>);
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
  OPERATOR 16 <%%> (geometry, geometry) FOR ORDER BY float_ops,
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

CREATE OR REPLACE FUNCTION laplace_schema_indexes() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  CREATE INDEX IF NOT EXISTS entity_hilbert ON entity (hilbert);
  CREATE INDEX IF NOT EXISTS entity_coord ON entity USING gist (coord laplace_point4d_ops);
  -- 32 MB a partition: a search scans the pending list, so it spills around a batch instead of holding a source.
  -- 4 MB spilled every few thousand paths (full-page images). 256 MB held the list until the source ended.
  CREATE INDEX IF NOT EXISTS physicality_paths ON physicality USING gin (path laplace_path_ops, mask laplace_mask_ops) WITH (gin_pending_list_limit = 32768);
  CREATE INDEX IF NOT EXISTS attestation_witness ON attestation (witness);
END $$;
DROP INDEX IF EXISTS entity_coord;
SELECT laplace_schema_indexes();
