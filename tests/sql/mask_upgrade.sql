\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
DROP EXTENSION laplace CASCADE;
CREATE EXTENSION laplace VERSION '1.13';
-- Populate the previous on-disk representation without invoking the candidate's
-- new 64-bit GIN support on the old 256-bit column.
DROP INDEX physicality_paths;
INSERT INTO entity (id, tier, coord, hilbert)
VALUES (laplace_id('upgrade'), laplace_tier('upgrade'), laplace_coord('upgrade'), laplace_hilbert('upgrade'));
INSERT INTO physicality (entity, tier, hilbert, path, mask)
VALUES (laplace_id('upgrade'), laplace_tier('upgrade'), laplace_hilbert('upgrade'),
        laplace_path(ARRAY[laplace_cp_id(117), laplace_cp_id(112)]), B'1011'::bit(256));
ALTER EXTENSION laplace UPDATE TO '1.14';
CREATE TEMP TABLE upgrade_paths (path geometry, keep boolean);
INSERT INTO upgrade_paths VALUES (laplace_path(ARRAY[laplace_cp_id(97),laplace_cp_id(98),laplace_cp_id(99)]), true);
CREATE INDEX upgrade_paths_gin ON upgrade_paths USING gin(path laplace_path_ops) WHERE keep;
COMMENT ON INDEX upgrade_paths_gin IS 'preserve this partial index';
ALTER EXTENSION laplace UPDATE;
DO $$
BEGIN
  IF NOT (SELECT bit_length(mask) = 64 AND laplace_mask_valid(mask)
          AND laplace_mask_has_value(mask, 'kind', 'claim')
          AND laplace_mask_has_value(mask, 'kind', 'tuple')
          AND laplace_mask_has_value(mask, 'kind', 'file')
          AND NOT laplace_mask_has_value(mask, 'kind', 'record')
          FROM physicality WHERE entity = laplace_id('upgrade'))
     OR NOT (SELECT mask = B'0'::bit(64) FROM entity WHERE id = laplace_id('upgrade')) THEN
    RAISE EXCEPTION 'upgrade lost existing carrier roles or initialized entity masks incorrectly';
  END IF;
  IF NOT (SELECT indisvalid FROM pg_index WHERE indexrelid = 'physicality_paths'::regclass)
     OR (SELECT count(*) FROM pg_inherits WHERE inhparent = 'physicality_paths'::regclass) <> 256
     OR obj_description('upgrade_paths_gin'::regclass, 'pg_class') <> 'preserve this partial index'
     OR NOT (SELECT path ^@ laplace_cp_id(97) AND path @^ laplace_cp_id(99) FROM upgrade_paths) THEN
    RAISE EXCEPTION 'upgrade did not preserve partitioned or user-created path indexes';
  END IF;
END $$;
DELETE FROM physicality WHERE entity = laplace_id('upgrade');
DELETE FROM entity WHERE id = laplace_id('upgrade');
SELECT 'mask_upgrade: existing role flags preserved in 64-bit carriers';
