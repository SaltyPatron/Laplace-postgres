-- Laplace content schema (PostgreSQL 18 + PostGIS + laplace).
--
-- Entities partition by tier, and the large tiers again by the leading bits of their ID. An ID is a BLAKE3 hash, so
-- ID prefixes split any content, in any script or language, into equal parts, and a lookup by ID prunes to exactly one
-- partition. Partitions are not spatial: the 4D side is served by GiST on the real coordinates, whose index structure
-- follows the content wherever it lies in the 4-ball, and the Hilbert value is a column and an index for ordering.
--
-- IDs are uuid: 16 fixed bytes. Hilbert values are unsigned 64-bit, stored with the top bit flipped
-- (hilbert # -9223372036854775808) so that bigint order equals Hilbert order.

CREATE TABLE entity (
  id       uuid     NOT NULL,
  tier     smallint NOT NULL,
  coord    geometry(PointZM) NOT NULL,           -- the real 4D coordinate; M is W
  hilbert  bigint   NOT NULL
) PARTITION BY LIST (tier);

CREATE TABLE physicality (
  entity   uuid     NOT NULL,
  tier     smallint NOT NULL,
  hilbert  bigint   NOT NULL,
  path     geometry NOT NULL                     -- children's IDs in X/Y/Z, run lengths in M
) PARTITION BY LIST (tier);

CREATE TABLE source (
  trunk    uuid   NOT NULL,
  origin   text   NOT NULL,
  format   text   NOT NULL,
  bytes    bigint NOT NULL,
  content  bytea  NOT NULL                       -- BLAKE3-256 of the source bytes
);

-- Every tier is split 16 ways by the first hex digit of the ID: an ID is a hash, so every part fills evenly, a lookup
-- by ID prunes to one partition, and a load writes every part on its own connection. Tiers deeper than 15 share the
-- default partition, split the same way.
DO $$
DECLARE t int; k int; lo text; hi text;
BEGIN
  FOR t IN 0..16 LOOP
    IF t < 16 THEN
      EXECUTE format('CREATE TABLE entity_t%s PARTITION OF entity FOR VALUES IN (%s) PARTITION BY RANGE (id)', t, t);
      EXECUTE format('CREATE TABLE physicality_t%s PARTITION OF physicality FOR VALUES IN (%s) PARTITION BY RANGE (entity)', t, t);
    ELSE
      CREATE TABLE entity_tx PARTITION OF entity DEFAULT PARTITION BY RANGE (id);
      CREATE TABLE physicality_tx PARTITION OF physicality DEFAULT PARTITION BY RANGE (entity);
    END IF;
    FOR k IN 0..15 LOOP                          -- the first hex digit of the ID
      lo := CASE WHEN k = 0 THEN 'MINVALUE' ELSE quote_literal(to_hex(k) || '0000000-0000-0000-0000-000000000000') END;
      hi := CASE WHEN k = 15 THEN 'MAXVALUE' ELSE quote_literal(to_hex(k + 1) || '0000000-0000-0000-0000-000000000000') END;
      IF t < 16 THEN
        EXECUTE format('CREATE TABLE entity_t%s_%s PARTITION OF entity_t%s FOR VALUES FROM (%s) TO (%s)', t, to_hex(k), t, lo, hi);
        EXECUTE format('CREATE TABLE physicality_t%s_%s PARTITION OF physicality_t%s FOR VALUES FROM (%s) TO (%s)', t, to_hex(k), t, lo, hi);
      ELSE
        EXECUTE format('CREATE TABLE entity_tx_%s PARTITION OF entity_tx FOR VALUES FROM (%s) TO (%s)', to_hex(k), lo, hi);
        EXECUTE format('CREATE TABLE physicality_tx_%s PARTITION OF physicality_tx FOR VALUES FROM (%s) TO (%s)', to_hex(k), lo, hi);
      END IF;
    END LOOP;
  END LOOP;
END $$;

-- A path's X/Y/Z are packed IDs, not positions, so PostGIS's geometry statistics mean nothing for them and cost minutes
-- (5.5 min on 3.9M paths, against 2.5 s without). The planner does not need them.
ALTER TABLE physicality ALTER COLUMN path SET STATISTICS 0;

-- Container lookups take a millisecond; starting parallel workers for one takes 15 (measured). No parallel scans of
-- paths, and no parallel append across partitions: the database default, which an analytic session can turn back on.
DO $$
DECLARE r record;
BEGIN
  FOR r IN SELECT c.relname FROM pg_class c JOIN pg_inherits i ON i.inhrelid = c.oid
           WHERE c.relname LIKE 'physicality_t%' AND c.relkind = 'r' LOOP
    EXECUTE format('ALTER TABLE %I SET (parallel_workers = 0)', r.relname);
  END LOOP;
  EXECUTE format('ALTER DATABASE %I SET enable_parallel_append = off', current_database());
END $$;
