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
-- Complete set validation must still reject identity conflicts and missing
-- partner rows, leaving the caller's copied batch available for correction.
DO $$ BEGIN
  PERFORM pg_temp.fill_stage();
  UPDATE stage.merge_entities SET hilbert = hilbert + 1 WHERE id = laplace_id('dog');
  BEGIN
    PERFORM laplace_merge_stage('stage.merge_entities', 'stage.merge_paths');
    RAISE EXCEPTION 'conflicting identity accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
  IF (SELECT count(*) FROM stage.merge_entities) <> 3 THEN
    RAISE EXCEPTION 'identity rejection consumed pending input';
  END IF;
  TRUNCATE stage.merge_entities, stage.merge_paths;
  INSERT INTO stage.merge_entities (id, tier, coord, hilbert)
    SELECT laplace_id('new'), laplace_tier('new'), laplace_coord('new'), laplace_hilbert('new');
  BEGIN
    PERFORM laplace_merge_stage('stage.merge_entities', 'stage.merge_paths');
    RAISE EXCEPTION 'unpaired entity accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
  IF (SELECT count(*) FROM stage.merge_entities) <> 1 THEN
    RAISE EXCEPTION 'pair rejection consumed pending input';
  END IF;
END $$;
DO $$ BEGIN
  BEGIN
    PERFORM laplace_merge_stage('public.entity', 'stage.merge_paths');
    RAISE EXCEPTION 'public input table accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
-- Every successfully merged batch must be ready for indexed containment
-- reads, without paying a pending-list scan at every provenance frontier.
DO $$ DECLARE remaining bigint; cleaned bigint; untouched bigint; BEGIN
  SELECT coalesce(sum(pg_catalog.gin_clean_pending_list(i.indexrelid)),0) INTO remaining
  FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid JOIN pg_am a ON a.oid=c.relam
  WHERE a.amname='gin' AND i.indrelid IN
    (SELECT tableoid FROM public.physicality
     WHERE entity=ANY(ARRAY[laplace_id('dog'),laplace_id('cat'),laplace_id('ant')]));
  IF remaining<>0 THEN RAISE EXCEPTION 'stage merge left pending GIN entries'; END IF;
  INSERT INTO public.entity (id,tier,coord,hilbert)
    SELECT laplace_id(t),laplace_tier(t),laplace_coord(t),laplace_hilbert(t)
    FROM unnest(ARRAY['touched','unrelated']) t;
  INSERT INTO public.physicality (entity,tier,hilbert,path)
    SELECT laplace_id(t),laplace_tier(t),laplace_hilbert(t),
      laplace_path(ARRAY(SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table(t,'') c))
    FROM unnest(ARRAY['touched','unrelated']) t;
  IF left(laplace_id('touched')::text,2)=left(laplace_id('unrelated')::text,2) THEN
    RAISE EXCEPTION 'maintenance fixture requires distinct destination partitions';
  END IF;
  CREATE UNLOGGED TABLE stage.maintenance_paths (LIKE stage.physicality INCLUDING DEFAULTS);
  INSERT INTO stage.maintenance_paths SELECT * FROM public.physicality WHERE entity=laplace_id('touched');
  cleaned := laplace_clean_stage('stage.maintenance_paths');
  SELECT coalesce(sum(pg_catalog.gin_clean_pending_list(i.indexrelid)),0) INTO untouched
  FROM pg_index i JOIN public.physicality p ON p.tableoid=i.indrelid
    JOIN pg_class c ON c.oid=i.indexrelid JOIN pg_am a ON a.oid=c.relam
  WHERE a.amname='gin' AND p.entity=laplace_id('unrelated');
  IF cleaned<=0 OR untouched<=0 OR laplace_clean_stage('stage.maintenance_paths')<>0 THEN
    RAISE EXCEPTION 'maintenance failed to drain copied partitions or swept unrelated indexes';
  END IF;
  TRUNCATE stage.maintenance_paths;
  IF laplace_clean_stage('stage.maintenance_paths')<>0 THEN
    RAISE EXCEPTION 'empty copied paths caused index maintenance';
  END IF;
  BEGIN
    PERFORM laplace_clean_stage('public.physicality');
    RAISE EXCEPTION 'maintenance accepted public input';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
-- A client's positive classifications survive staging. Only claimed-new keys
-- participate in the final flat overlap check, including ordinary replay.
TRUNCATE stage.merge_entities,stage.merge_paths;
ALTER TABLE stage.merge_entities ADD COLUMN present boolean NOT NULL DEFAULT true;
ALTER TABLE stage.merge_paths ADD COLUMN present boolean NOT NULL DEFAULT true;
DO $$ BEGIN PERFORM pg_temp.fill_stage(); END $$;
DO $$ DECLARE result record; BEGIN
  IF EXISTS(SELECT FROM laplace_present_stage('stage.merge_entities','public.entity','id','present'))
     OR EXISTS(SELECT FROM laplace_present_stage('stage.merge_paths','public.physicality','entity','present')) THEN
    RAISE EXCEPTION 'positive client classifications were probed again';
  END IF;
  SELECT * INTO result FROM laplace_merge_stage('stage.merge_entities','stage.merge_paths');
  IF result.entity_rows<>0 OR result.physicality_rows<>0 OR result.updated_rows<>0 THEN
    RAISE EXCEPTION 'classified replay changed canonical rows';
  END IF;
  IF EXISTS(SELECT FROM pg_locks WHERE pid=pg_backend_pid() AND locktype='advisory'
            AND classid=1279348803::oid AND objid=4294967294::oid) THEN
    RAISE EXCEPTION 'structural merge retained a global apply lock';
  END IF;
END $$;
DO $$ BEGIN PERFORM pg_temp.fill_stage(); END $$;
UPDATE stage.merge_entities SET present=false;
UPDATE stage.merge_paths SET present=false;
DO $$ DECLARE result record; BEGIN
  IF (SELECT count(*) FROM laplace_present_stage('stage.merge_entities','public.entity','id','present'))<>3
     OR (SELECT count(*) FROM laplace_present_stage('stage.merge_paths','public.physicality','entity','present'))<>3 THEN
    RAISE EXCEPTION 'flat verification lost an overlapping canonical ID';
  END IF;
  SELECT * INTO result FROM laplace_merge_stage('stage.merge_entities','stage.merge_paths');
  IF result.entity_rows<>0 OR result.physicality_rows<>0 OR result.updated_rows<>0 THEN
    RAISE EXCEPTION 'overlap verification inserted duplicate canonical rows';
  END IF;
  IF to_regclass(format('stage.lp_plan_%s_%s','stage.merge_entities'::regclass::oid,'stage.merge_paths'::regclass::oid)) IS NOT NULL THEN
    RAISE EXCEPTION 'merge retained its private classification plan';
  END IF;
END $$;
-- Existing content needs no coordinate/Hilbert retransmission. A metadata-only
-- update keeps the recorded path while adding a genuinely new carrier role.
ALTER TABLE stage.merge_entities ALTER coord DROP NOT NULL,ALTER hilbert DROP NOT NULL;
ALTER TABLE stage.merge_paths ALTER path DROP NOT NULL,ALTER hilbert DROP NOT NULL;
DO $$ BEGIN PERFORM pg_temp.fill_stage(); END $$;
UPDATE stage.merge_entities SET coord=NULL,hilbert=NULL;
UPDATE stage.merge_paths SET path=NULL,hilbert=NULL,mask=laplace_mask_slot('kind','record');
DO $$ DECLARE result record; BEGIN
  SELECT * INTO result FROM laplace_merge_stage('stage.merge_entities','stage.merge_paths');
  IF result.entity_rows<>0 OR result.physicality_rows<>0 OR result.updated_rows<>3
     OR EXISTS(SELECT FROM unnest(ARRAY['dog','cat','ant']) t JOIN public.physicality p ON p.entity=laplace_id(t)
       WHERE NOT laplace_mask_has_value(p.mask,'kind','record')
          OR laplace_text(p.entity)<>t) THEN
    RAISE EXCEPTION 'metadata-only update changed content or lost its new role';
  END IF;
END $$;
-- The bulk publisher owns its FK plan policy, never the caller's session.
-- PostgreSQL must restore even an explicitly selected caller policy after
-- successful publication and after a rejected identity payload.
DO $$ DECLARE original text:=current_setting('plan_cache_mode'); policy text; BEGIN
 FOREACH policy IN ARRAY ARRAY['auto','force_custom_plan','force_generic_plan'] LOOP
  PERFORM set_config('plan_cache_mode',policy,true);
  PERFORM pg_temp.fill_stage();
  PERFORM laplace_merge_stage('stage.merge_entities','stage.merge_paths');
  IF current_setting('plan_cache_mode')<>policy THEN RAISE EXCEPTION 'bulk publication leaked its plan policy'; END IF;
  PERFORM pg_temp.fill_stage();
  UPDATE stage.merge_entities SET hilbert=hilbert+1;
  BEGIN
   PERFORM laplace_merge_stage('stage.merge_entities','stage.merge_paths');
   RAISE EXCEPTION 'bulk publication accepted conflicting identity';
  EXCEPTION WHEN check_violation THEN NULL; END;
  IF current_setting('plan_cache_mode')<>policy THEN RAISE EXCEPTION 'failed publication leaked its plan policy'; END IF;
  TRUNCATE stage.merge_entities,stage.merge_paths;
 END LOOP;
 PERFORM set_config('plan_cache_mode',original,true);
END $$;
ROLLBACK;
