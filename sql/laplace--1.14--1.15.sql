-- Full ZM membership and endpoint postings use disjoint, exact 17-byte keys.
\echo Use "ALTER EXTENSION laplace UPDATE TO '1.15'" to load this file. \quit

CREATE FUNCTION laplace_path_starts(geometry, blake3) RETURNS boolean
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10000;
CREATE FUNCTION laplace_path_ends(geometry, blake3) RETURNS boolean
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10000;
CREATE OPERATOR ^@ (LEFTARG = geometry, RIGHTARG = blake3, FUNCTION = laplace_path_starts,
  RESTRICT = contsel, JOIN = contjoinsel);
CREATE OPERATOR @^ (LEFTARG = geometry, RIGHTARG = blake3, FUNCTION = laplace_path_ends,
  RESTRICT = contsel, JOIN = contjoinsel);

-- Rebuild every affected index, preserving names, comments, partial/expression
-- definitions and the exact partition attachment tree, including incomplete
-- partitioned indexes. Working state stays in this block, never in a table.
DO $$
DECLARE names text[]; definitions text[]; descriptions text[]; parents text[]; i integer;
BEGIN
  WITH RECURSIVE affected AS (
    SELECT c.oid, c.relispartition FROM pg_index x JOIN pg_class c ON c.oid = x.indexrelid
    WHERE (SELECT oid FROM pg_opclass WHERE opcname = 'laplace_path_ops'
           AND opcnamespace = (SELECT extnamespace FROM pg_extension WHERE extname = 'laplace')
           AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'gin')) = ANY(x.indclass::oid[])
  ), tree AS (
    SELECT a.oid, NULL::oid AS parent, 0 AS depth FROM affected a WHERE NOT a.relispartition
    UNION ALL
    SELECT a.oid, t.oid, t.depth + 1 FROM tree t JOIN pg_inherits h ON h.inhparent = t.oid
      JOIN affected a ON a.oid = h.inhrelid
  )
  SELECT array_agg(oid::regclass::text ORDER BY depth, oid), array_agg(pg_get_indexdef(oid) ORDER BY depth, oid),
         array_agg(obj_description(oid, 'pg_class') ORDER BY depth, oid), array_agg(parent::regclass::text ORDER BY depth, oid)
  INTO names, definitions, descriptions, parents FROM tree;
  FOR i IN 1..coalesce(cardinality(names), 0) LOOP
    IF parents[i] IS NULL THEN EXECUTE format('DROP INDEX %s', names[i]); END IF;
  END LOOP;
  DROP OPERATOR CLASS laplace_path_ops USING gin;
  DROP OPERATOR FAMILY laplace_path_ops USING gin;
  CREATE OPERATOR CLASS laplace_path_ops FOR TYPE geometry USING gin AS
  OPERATOR 3 && (geometry, blake3[]),
  OPERATOR 7 @> (geometry, blake3[]),
  OPERATOR 8 ^@ (geometry, blake3),
  OPERATOR 9 @^ (geometry, blake3),
  FUNCTION 1 byteacmp(bytea, bytea),
  FUNCTION 2 laplace_gin_extract_value(geometry, internal, internal),
  FUNCTION 3 laplace_gin_extract_query(blake3[], internal, int2, internal, internal, internal, internal),
  FUNCTION 4 laplace_gin_consistent(internal, int2, blake3[], int4, internal, internal, internal, internal),
  FUNCTION 6 laplace_gin_triconsistent(internal, int2, blake3[], int4, internal, internal, internal),
    STORAGE bytea;
  FOR i IN 1..coalesce(cardinality(names), 0) LOOP
    EXECUTE definitions[i];
    IF descriptions[i] IS NOT NULL THEN EXECUTE format('COMMENT ON INDEX %s IS %L', names[i], descriptions[i]); END IF;
  END LOOP;
  FOR i IN REVERSE coalesce(cardinality(names), 0)..1 LOOP
    IF parents[i] IS NOT NULL THEN EXECUTE format('ALTER INDEX %s ATTACH PARTITION %s', parents[i], names[i]); END IF;
  END LOOP;
END $$;
