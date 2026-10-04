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
