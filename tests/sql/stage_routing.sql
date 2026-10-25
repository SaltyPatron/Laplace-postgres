\set ECHO none
\pset format unaligned
\pset tuples_only on
BEGIN;
SET LOCAL work_mem = '64kB';
SELECT laplace_ensure_stage();
CREATE TEMP TABLE routing_words AS
SELECT 'stageenrich' || chr(97+i%26) || chr(97+(i/26)%26) || chr(97+(i/676)%26) AS content
FROM generate_series(0,2048) i;
CREATE UNLOGGED TABLE stage.routing_entities (LIKE stage.entity INCLUDING DEFAULTS);
CREATE UNLOGGED TABLE stage.routing_paths (LIKE stage.physicality INCLUDING DEFAULTS);
CREATE FUNCTION pg_temp.routing_batch() RETURNS void LANGUAGE SQL AS $$
  INSERT INTO stage.routing_entities(id,tier,coord,hilbert)
    SELECT laplace_id(content),laplace_tier(content),laplace_coord(content),laplace_hilbert(content) FROM routing_words;
  INSERT INTO stage.routing_paths(entity,tier,hilbert,path)
    SELECT laplace_id(content),laplace_tier(content),laplace_hilbert(content),
      laplace_path(ARRAY(SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table(content,'') c)) FROM routing_words;
$$;
SELECT pg_temp.routing_batch();
DO $$ DECLARE result record; BEGIN
  SELECT * INTO result FROM laplace_merge_stage('stage.routing_entities','stage.routing_paths');
  IF result.entity_rows<>2049 OR result.physicality_rows<>2049 OR result.updated_rows<>0 THEN
    RAISE EXCEPTION 'partition routing changed insertion counts: %',result;
  END IF;
END $$;
SELECT pg_temp.routing_batch();
UPDATE stage.routing_entities SET mask=laplace_mask_slot('kind','record');
UPDATE stage.routing_paths SET mask=laplace_mask_slot('kind','tuple');
-- Parent UPDATE privileges must suffice even though routing prunes child
-- targets. The maintenance-test role has no UPDATE grant on any child.
GRANT USAGE ON SCHEMA stage TO :"writer";
GRANT SELECT ON stage.routing_entities,stage.routing_paths TO :"writer";
GRANT SELECT,UPDATE ON public.entity,public.physicality TO :"writer";
SET SESSION AUTHORIZATION :"writer";
DO $$ BEGIN
  IF laplace_enrich_stage('stage.routing_entities','public.entity')<>2049
     OR laplace_enrich_stage('stage.routing_paths','public.physicality')<>2049 THEN
    RAISE EXCEPTION 'small-memory routing lost a changed carrier';
  END IF;
END $$;
RESET SESSION AUTHORIZATION;
DO $$ BEGIN
  IF EXISTS (SELECT FROM routing_words w JOIN public.entity e ON e.id=laplace_id(w.content)
             JOIN public.physicality p ON p.entity=e.id
             WHERE NOT laplace_mask_has_value(e.mask,'kind','record')
                OR NOT laplace_mask_has_value(p.mask,'kind','tuple')
                OR ST_AsEWKB(p.path)<>ST_AsEWKB(laplace_path(ARRAY(
                  SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table(w.content,'') c)))) THEN
    RAISE EXCEPTION 'routed enrichment lost role flags or changed canonical geometry';
  END IF;
END $$;
-- Column UPDATE/SELECT grants on the parent remain sufficient as well.
REVOKE SELECT,UPDATE ON public.entity FROM :"writer";
GRANT SELECT(id,tier,mask),UPDATE(tier,mask) ON public.entity TO :"writer";
UPDATE stage.routing_entities SET mask=mask|laplace_mask_slot('kind','tuple');
SET SESSION AUTHORIZATION :"writer";
DO $$ BEGIN
  IF laplace_enrich_stage('stage.routing_entities','public.entity')<>2049 THEN
    RAISE EXCEPTION 'parent column grants were not preserved';
  END IF;
END $$;
RESET SESSION AUTHORIZATION;
REVOKE UPDATE(tier,mask) ON public.entity FROM :"writer";
GRANT SELECT ON public.entity TO :"writer";
TRUNCATE stage.routing_entities,stage.routing_paths;
SELECT pg_temp.routing_batch();
DO $$ DECLARE result record; BEGIN
  SELECT * INTO result FROM laplace_merge_stage('stage.routing_entities','stage.routing_paths');
  IF result.entity_rows<>0 OR result.physicality_rows<>0 OR result.updated_rows<>0 THEN
    RAISE EXCEPTION 'routed replay changed existing carriers: %',result;
  END IF;
  BEGIN
    PERFORM laplace_enrich_stage('public.entity','public.entity');
    RAISE EXCEPTION 'public input accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    PERFORM laplace_enrich_stage('stage.routing_entities','stage.routing_paths');
    RAISE EXCEPTION 'noncanonical destination accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
CREATE UNLOGGED TABLE stage.routing_wrong_type(id text,tier smallint,mask bit(64));
CREATE UNLOGGED TABLE stage.routing_null_id AS SELECT NULL::blake3 AS id,1::smallint AS tier,B'0'::bit(64) AS mask;
DO $$ BEGIN
  BEGIN
    PERFORM laplace_enrich_stage('stage.routing_wrong_type','public.entity');
    RAISE EXCEPTION 'non-Native ID type accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    IF laplace_enrich_stage('stage.routing_null_id','public.entity')<>0 THEN
      RAISE EXCEPTION 'null input acquired a destination';
    END IF;
  EXCEPTION WHEN null_value_not_allowed THEN NULL; END;
END $$;
REVOKE UPDATE ON public.entity,public.physicality FROM :"writer";
SET SESSION AUTHORIZATION :"writer";
DO $$ BEGIN
  BEGIN
    PERFORM laplace_enrich_stage('stage.routing_entities','public.entity');
    RAISE EXCEPTION 'unchanged input bypassed destination UPDATE privileges';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
END $$;
RESET SESSION AUTHORIZATION;
ROLLBACK;
SELECT 'stage routing: 2049 carriers, small-memory windows, parent privileges and exact replay passed';
