-- Laplace-postgres 0.4 → 0.5: how many times a path holds each of a set of IDs, runs included, so a caller walking
-- the DAG receives counts instead of whole paths.
\echo Use "ALTER EXTENSION laplace UPDATE TO '0.5'" to load this file. \quit
CREATE FUNCTION laplace_path_times(geometry, uuid[], OUT id uuid, OUT times bigint) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_path_times' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000 ROWS 4;
