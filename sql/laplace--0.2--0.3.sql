-- Laplace-postgres 0.2 → 0.3: selectivity for the path operators. A path holds few of all IDs, so the planner is told
-- containment and overlap are selective and reaches for the index instead of decoding whole partitions.
\echo Use "ALTER EXTENSION laplace UPDATE TO '0.3'" to load this file. \quit
ALTER OPERATOR @> (geometry, uuid[]) SET (RESTRICT = contsel, JOIN = contjoinsel);
ALTER OPERATOR && (geometry, uuid[]) SET (RESTRICT = contsel, JOIN = contjoinsel);
