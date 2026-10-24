\echo Use "ALTER EXTENSION laplace UPDATE TO '1.29'" to load this file. \quit

-- The Native operator caches a query ID set and searches by path vertex.
-- Retire the old per-row frontier-loop penalty: it forced large GIN key
-- searches even for tiny tables. Let the planner compare normal native
-- execution with the existing exact GIN postings; no index is forced.
ALTER FUNCTION laplace_path_contains(geometry, blake3[]) COST 1;
ALTER FUNCTION laplace_path_overlaps(geometry, blake3[]) COST 1;
