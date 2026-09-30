-- Laplace 1.0 -> 1.1: the web functions.
\echo Use "ALTER EXTENSION laplace UPDATE" to load this file. \quit

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
