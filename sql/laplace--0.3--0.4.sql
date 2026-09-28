-- Laplace-postgres 0.3 → 0.4: the path operators decode a whole path when evaluated outside the index (a file's trunk
-- holds every paragraph), so they carry that cost and the planner uses the index even on small partitions.
\echo Use "ALTER EXTENSION laplace UPDATE TO '0.4'" to load this file. \quit
ALTER FUNCTION laplace_path_contains(geometry, uuid[]) COST 10000;
ALTER FUNCTION laplace_path_overlaps(geometry, uuid[]) COST 10000;
