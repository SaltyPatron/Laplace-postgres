\echo Use "ALTER EXTENSION laplace UPDATE TO '1.33'" to load this file. \quit

-- NULL is PostgreSQL's unbounded LIMIT, passed through to the Native set read.
-- Other NULL predicates still have their normal SQL meaning; no stand-in fan
-- or integer ceiling is substituted by a caller.
ALTER FUNCTION laplace_claims(blake3[],bigint,smallint[],blake3[]) CALLED ON NULL INPUT;
ALTER FUNCTION laplace_claims_each(blake3[],bigint,smallint[],blake3[]) CALLED ON NULL INPUT;
COMMENT ON FUNCTION laplace_claims_each(blake3[],bigint,smallint[],blake3[]) IS
  'Native indexed claim set for every input occurrence, with the caller-selected fan or NULL for all responders. Virtual Tier-0 identities need no entity row.';

CREATE FUNCTION laplace_text_batch(ids blake3[],OUT i bigint,OUT entity blake3,OUT content text)
RETURNS SETOF record AS 'MODULE_PATHNAME','laplace_text_batch'
LANGUAGE C STABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION laplace_text_batch(blake3[]) IS
  'Realize selected Native identities together, preserving input occurrences and runs while reading shared DAG descendants once. Transfer windows follow work_mem and PostgreSQL array capacity.';

CREATE FUNCTION laplace_labels(ids blake3[],OUT i bigint,OUT entity blake3,OUT content text)
RETURNS SETOF record AS 'MODULE_PATHNAME','laplace_labels'
LANGUAGE C STABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION laplace_labels(blake3[]) IS
  'Native selected-ID labels in one DAG context: Unicode text and ordered tuple/run structure. Other structural roles and missing realizations retain their canonical identity; package bodies are not reconstructed to label them.';
