-- Laplace 1.3 -> 1.4: the forward pass over every segment of a prompt, as one set.
\echo Use "ALTER EXTENSION laplace UPDATE" to load this file. \quit

-- laplace_forward(ids, fan): for every contiguous segment [i..j] of the prompt's constituents, the observations holding all
-- of its parts (at most fan, claims left out), how many hold it as a run, and what follows the run in each, counted:
-- one row per continuation; a segment held by nothing, one row with next null. Precedes, contains and co-occurrence
-- from the trajectories (Storage: Physicality), every segment at once (Semantics: Pull).
CREATE FUNCTION laplace_forward(ids blake3[], fan bigint, OUT i integer, OUT j integer, OUT paths bigint, OUT runs bigint, OUT next blake3, OUT times bigint)
RETURNS SETOF record AS 'MODULE_PATHNAME', 'laplace_forward' LANGUAGE C STABLE STRICT PARALLEL SAFE;
