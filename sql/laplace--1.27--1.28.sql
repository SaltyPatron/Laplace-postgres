\echo Use "ALTER EXTENSION laplace UPDATE TO '1.28'" to load this file. \quit

CREATE FUNCTION laplace_contained_series(trunks blake3[],
  OUT trunk blake3, OUT claim blake3, OUT games double precision, OUT tokens double precision,
  OUT score double precision, OUT "position" integer, OUT dependence_root blake3,
  OUT dependence_trust double precision) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_contained_series'
  LANGUAGE C STABLE STRICT PARALLEL SAFE SET search_path = public, pg_temp ROWS 256;
COMMENT ON FUNCTION laplace_contained_series(blake3[]) IS
  'Read Native claim series below persisted trunks using bounded SPI frontier sets. Shares edge, RLE, outcome and dependence-class tallying with upward containment; no source bytes, standings mutation or witness publication.';
