\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
BEGIN;
SELECT laplace_ensure_stage();
CREATE UNLOGGED TABLE stage.merge_entities (LIKE stage.entity INCLUDING DEFAULTS);
CREATE UNLOGGED TABLE stage.merge_paths (LIKE stage.physicality INCLUDING DEFAULTS);
CREATE UNLOGGED TABLE stage.other_run (id integer);
INSERT INTO stage.other_run VALUES (42);
CREATE FUNCTION pg_temp.fill_stage() RETURNS void LANGUAGE SQL AS $$
  INSERT INTO stage.merge_entities (id, tier, coord, hilbert)
    SELECT laplace_id(t), laplace_tier(t), laplace_coord(t), laplace_hilbert(t)
    FROM unnest(ARRAY['dog','cat','ant']) t;
  INSERT INTO stage.merge_paths (entity, tier, hilbert, path)
    SELECT laplace_id(t), laplace_tier(t), laplace_hilbert(t),
      laplace_path(ARRAY(SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table(t, '') c))
    FROM unnest(ARRAY['dog','cat','ant']) t;
$$;
SELECT pg_temp.fill_stage();
SELECT entity_rows = 3 AND physicality_rows = 3 AND updated_rows = 0 AS inserted
FROM laplace_merge_stage('stage.merge_entities', 'stage.merge_paths');
SELECT string_agg(laplace_text(laplace_id(t)), ',' ORDER BY t) = 'ant,cat,dog' AS reconstructed
FROM unnest(ARRAY['dog','cat','ant']) t;
SELECT NOT EXISTS (SELECT FROM stage.merge_entities) AND NOT EXISTS (SELECT FROM stage.merge_paths)
  AND (SELECT id FROM stage.other_run) = 42 AS isolated;
SELECT laplace_ensure_stage();
SELECT pg_temp.fill_stage();
SELECT entity_rows = 0 AND physicality_rows = 0 AND updated_rows = 0 AS replay
FROM laplace_merge_stage('stage.merge_entities', 'stage.merge_paths');
-- A failure midway through merge must preserve all copied input and public
-- state, rather than leaving one side of an entity/path pair committed.
DO $$ BEGIN
  PERFORM pg_temp.fill_stage();
  INSERT INTO stage.merge_entities SELECT * FROM stage.merge_entities LIMIT 1;
  BEGIN
    PERFORM laplace_merge_stage('stage.merge_entities', 'stage.merge_paths');
    RAISE EXCEPTION 'duplicate stage IDs accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
  IF (SELECT count(*) FROM stage.merge_entities) <> 4 OR (SELECT count(*) FROM stage.merge_paths) <> 3 THEN
    RAISE EXCEPTION 'failed batch consumed pending input';
  END IF;
END $$;
TRUNCATE stage.merge_entities, stage.merge_paths;
SELECT pg_temp.fill_stage();
UPDATE stage.merge_paths SET mask = laplace_mask_slot('kind', 'tuple');
SELECT entity_rows = 0 AND physicality_rows = 0 AND updated_rows = 3 AS carriers_merged
FROM laplace_merge_stage('stage.merge_entities', 'stage.merge_paths');
SELECT bool_and(laplace_mask_has_value(p.mask, 'kind', 'tuple')) AS carriers_preserved
FROM physicality p WHERE p.entity = ANY(ARRAY[laplace_id('dog'),laplace_id('cat'),laplace_id('ant')]);
DO $$ BEGIN
  BEGIN
    PERFORM laplace_merge_stage('public.entity', 'stage.merge_paths');
    RAISE EXCEPTION 'public input table accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
ROLLBACK;
