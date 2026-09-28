-- Laplace-postgres 0.1 → 0.2: GIN over path geometry. Nothing is dropped; existing indexes and data stay as they are.
\echo Use "ALTER EXTENSION laplace UPDATE TO '0.2'" to load this file. \quit

-- A path holds these IDs (all of them) / any of them: answered exactly from the index, with no recheck.
CREATE FUNCTION laplace_path_contains(geometry, uuid[]) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_path_contains' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_path_overlaps(geometry, uuid[]) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_path_overlaps' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR @> (LEFTARG = geometry, RIGHTARG = uuid[], FUNCTION = laplace_path_contains);
CREATE OPERATOR && (LEFTARG = geometry, RIGHTARG = uuid[], FUNCTION = laplace_path_overlaps);

CREATE FUNCTION laplace_gin_extract_value(geometry, internal, internal) RETURNS internal
  AS 'MODULE_PATHNAME', 'laplace_gin_extract_value' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_gin_extract_query(uuid[], internal, int2, internal, internal, internal, internal) RETURNS internal
  AS 'MODULE_PATHNAME', 'laplace_gin_extract_query' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_gin_consistent(internal, int2, uuid[], int4, internal, internal, internal, internal) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_gin_consistent' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_gin_triconsistent(internal, int2, uuid[], int4, internal, internal, internal) RETURNS "char"
  AS 'MODULE_PATHNAME', 'laplace_gin_triconsistent' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR CLASS laplace_path_ops FOR TYPE geometry USING gin AS
  OPERATOR 3 && (geometry, uuid[]),
  OPERATOR 7 @> (geometry, uuid[]),
  FUNCTION 1 uuid_cmp(uuid, uuid),
  FUNCTION 2 laplace_gin_extract_value(geometry, internal, internal),
  FUNCTION 3 laplace_gin_extract_query(uuid[], internal, int2, internal, internal, internal, internal),
  FUNCTION 4 laplace_gin_consistent(internal, int2, uuid[], int4, internal, internal, internal, internal),
  FUNCTION 6 laplace_gin_triconsistent(internal, int2, uuid[], int4, internal, internal, internal),
  STORAGE uuid;
