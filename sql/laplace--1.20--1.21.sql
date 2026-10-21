\echo Use "ALTER EXTENSION laplace UPDATE TO '1.21'" to load this file. \quit

-- Optional file-level diagnostics use the same Native containment operator.
-- Corpus standings continue to consume only rows marked as registered witnesses.
CREATE FUNCTION laplace_attested(claims blake3[], include_files boolean,
  OUT claim blake3, OUT trunk blake3, OUT games double precision, OUT tokens double precision, OUT score double precision,
  OUT "position" integer, OUT trust double precision, OUT witness boolean) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_attested' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 256;
