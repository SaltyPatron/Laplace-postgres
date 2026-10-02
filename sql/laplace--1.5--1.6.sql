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
