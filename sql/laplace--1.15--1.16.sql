-- Exact native 4D KNN; PostGIS's public 2D <-> keeps its own semantics.
\echo Use "ALTER EXTENSION laplace UPDATE TO '1.16'" to load this file. \quit
CREATE SCHEMA IF NOT EXISTS laplace;
CREATE OPERATOR laplace.<-> (LEFTARG = geometry, RIGHTARG = geometry,
  FUNCTION = laplace_frechet4d, COMMUTATOR = OPERATOR(laplace.<->));
COMMENT ON OPERATOR laplace.<-> (geometry, geometry) IS
  'Native 4D discrete Frechet distance, equal to chord distance for two points. Use OPERATOR(laplace.<->) or put laplace before public in search_path.';

CREATE OPERATOR CLASS laplace_coord_ops FOR TYPE geometry USING gist AS
  OPERATOR 15 laplace.<-> (geometry, geometry) FOR ORDER BY float_ops,
  FUNCTION 1 laplace_point4d_gist_consistent(internal, geometry, smallint, oid, internal),
  FUNCTION 2 laplace_point4d_gist_union(internal, internal),
  FUNCTION 3 laplace_point4d_gist_compress(internal),
  FUNCTION 5 laplace_point4d_gist_penalty(internal, internal, internal),
  FUNCTION 6 laplace_point4d_gist_picksplit(internal, internal),
  FUNCTION 7 laplace_point4d_gist_same(laplace_box4d, laplace_box4d, internal),
  FUNCTION 8 laplace_point4d_gist_distance(internal, geometry, smallint, oid, internal),
  FUNCTION 11 laplace_point4d_gist_sortsupport(internal),
  STORAGE laplace_box4d;
CREATE OPERATOR CLASS laplace_traj_ops FOR TYPE geometry USING gist AS
  OPERATOR 15 laplace.<-> (geometry, geometry) FOR ORDER BY float_ops,
  FUNCTION 1 laplace_point4d_gist_consistent(internal, geometry, smallint, oid, internal),
  FUNCTION 2 laplace_path4d_gist_union(internal, internal),
  FUNCTION 3 laplace_path4d_gist_compress(internal),
  FUNCTION 5 laplace_point4d_gist_penalty(internal, internal, internal),
  FUNCTION 6 laplace_path4d_gist_picksplit(internal, internal),
  FUNCTION 7 laplace_path4d_gist_same(laplace_path4d_key, laplace_path4d_key, internal),
  FUNCTION 8 laplace_path4d_gist_distance(internal, geometry, smallint, oid, internal),
  FUNCTION 11 laplace_point4d_gist_sortsupport(internal),
  STORAGE laplace_path4d_key;
