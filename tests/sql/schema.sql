\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
BEGIN;
DROP EXTENSION laplace CASCADE;
DROP EXTENSION postgis CASCADE;
CREATE SCHEMA "native tenant";
CREATE SCHEMA "gis tenant";
CREATE EXTENSION postgis SCHEMA "gis tenant";
CREATE EXTENSION laplace SCHEMA "native tenant";
CREATE TABLE public.entity(id integer);
CREATE TABLE public.physicality(entity integer);
INSERT INTO public.entity VALUES(17);
INSERT INTO "native tenant".entity(id,tier,coord,hilbert)
SELECT "native tenant".laplace_id('ab'),"native tenant".laplace_tier('ab'),
       "native tenant".laplace_coord('ab'),"native tenant".laplace_hilbert('ab');
INSERT INTO "native tenant".physicality(entity,tier,hilbert,path)
SELECT id,tier,hilbert,"native tenant".laplace_path(ARRAY["native tenant".laplace_cp_id(97),"native tenant".laplace_cp_id(98)])
FROM "native tenant".entity;
SET LOCAL search_path=pg_catalog,pg_temp;
DO $$ DECLARE id "native tenant".blake3:="native tenant".laplace_id('ab'); value text;
BEGIN
 IF "native tenant".laplace_text(id)<>'ab' THEN RAISE EXCEPTION 'installed Native text binding failed'; END IF;
 IF "native tenant".laplace_path_length("native tenant".laplace_path(ARRAY[id,id]))<>2 THEN
  RAISE EXCEPTION 'installed PostGIS function binding failed';
 END IF;
 IF pg_catalog.cardinality("native tenant".laplace_vertex_ids("native tenant".laplace_path(ARRAY[id,id])))<>1 THEN
  RAISE EXCEPTION 'installed PostGIS exporter binding failed';
 END IF;
 SELECT min(content) INTO STRICT value FROM "native tenant".laplace_text_batch(ARRAY[id,id]);
 IF value<>'ab' THEN RAISE EXCEPTION 'batch text changed'; END IF;
 IF pg_catalog.encode("native tenant".laplace_resolve_ref('ab'),'hex')<>id::text
    OR pg_catalog.encode("native tenant".laplace_resolve_ref(pg_catalog.upper(id::text)),'hex')<>id::text THEN
  RAISE EXCEPTION 'installed reference identity binding failed';
 END IF;
 IF (SELECT pg_catalog.array_agg(r.label ORDER BY r.ordinal)
     FROM "native tenant".laplace_resolve_refs(ARRAY[pg_catalog.decode(id::text,'hex'),pg_catalog.decode(id::text,'hex')],ARRAY[NULL,'caller surface']) r)
    IS DISTINCT FROM ARRAY['ab','caller surface'] THEN
  RAISE EXCEPTION 'installed reference batch binding failed';
 END IF;
 IF (SELECT count(*) FROM "native tenant".laplace_paths(ARRAY[id]))<>1
    OR (SELECT count(*) FROM "native tenant".laplace_present_ids(ARRAY[id,id]))<>2 THEN
  RAISE EXCEPTION 'installed set binding failed';
 END IF;
 IF EXISTS(SELECT FROM pg_proc p JOIN pg_depend d ON d.classid='pg_proc'::regclass AND d.objid=p.oid
  JOIN pg_extension e ON e.oid=d.refobjid AND d.refclassid='pg_extension'::regclass
  WHERE e.extname='laplace' AND d.deptype='e' AND NOT p.prosecdef AND p.proconfig IS NOT NULL) THEN
  RAISE EXCEPTION 'ordinary Native or SQL function retains per-call settings';
 END IF;
END $$;
-- The same installed-object boundary covers set publication and maintenance,
-- including ID equality inside dynamic SQL. Public shadow tables stay unrelated.
DO $$ BEGIN PERFORM "native tenant".laplace_ensure_stage(); END $$;
CREATE UNLOGGED TABLE stage.schema_entities (LIKE "native tenant".entity);
CREATE UNLOGGED TABLE stage.schema_paths (LIKE "native tenant".physicality);
INSERT INTO stage.schema_entities SELECT * FROM "native tenant".entity;
INSERT INTO stage.schema_paths SELECT * FROM "native tenant".physicality;
DO $$ DECLARE merged record; projected record; id "native tenant".blake3:="native tenant".laplace_id('ab'); BEGIN
 SELECT * INTO merged FROM "native tenant".laplace_merge_stage('stage.schema_entities','stage.schema_paths');
 IF merged.entity_rows<>0 OR merged.physicality_rows<>0 THEN RAISE EXCEPTION 'namespace replay inserted duplicate content'; END IF;
 PERFORM "native tenant".laplace_schema_indexes();
 SELECT * INTO projected FROM "native tenant".laplace_recoordinate(false);
 IF projected.entity_rows<>0 OR projected.physicality_rows<>0 THEN RAISE EXCEPTION 'namespace audit changed canonical placement'; END IF;
 UPDATE "native tenant".physicality SET mask="native tenant".laplace_mask_slot('kind','claim') WHERE entity OPERATOR("native tenant".=) id;
 IF (SELECT fact FROM "native tenant".laplace_claim_preview(ARRAY[id]))<>'b' THEN
  RAISE EXCEPTION 'namespace claim preview changed selected content';
 END IF;
 IF "native tenant".laplace_attest_series('stage.attestation',ARRAY[id])<>0 THEN
  RAISE EXCEPTION 'namespace publication created evidence';
 END IF;
END $$;
DO $$ BEGIN
 PERFORM count(*) FROM "native tenant".laplace_containers(ARRAY["native tenant".laplace_id('ab')],'{}'::smallint[]);
END $$;
ALTER SCHEMA "native tenant" RENAME TO "renamed native";
ALTER SCHEMA "gis tenant" RENAME TO "renamed gis";
DO $$ BEGIN
 IF (SELECT count(*) FROM "renamed native".laplace_containers(ARRAY["renamed native".laplace_id('ab')],'{}'::smallint[]))<>0
  OR "renamed native".laplace_text("renamed native".laplace_id('ab'))<>'ab'
  OR "renamed gis".ST_AsEWKB("renamed native".laplace_coord('ab')) IS DISTINCT FROM "renamed native".laplace_coord_ewkb('ab') THEN
  RAISE EXCEPTION 'namespace rename invalidated Native or parsed SQL bindings';
 END IF;
END $$;
ROLLBACK;
DO $$ BEGIN
 IF public.laplace_path_length(public.laplace_path(ARRAY[public.laplace_cp_id(97),public.laplace_cp_id(98)]))<>2 THEN
  RAISE EXCEPTION 'rolled-back extension namespace poisoned a cached PostGIS binding';
 END IF;
 IF pg_catalog.cardinality(public.laplace_vertex_ids(public.laplace_path(ARRAY[public.laplace_cp_id(97),public.laplace_cp_id(98)])))<>2 THEN
  RAISE EXCEPTION 'rolled-back namespace poisoned a cached PostGIS exporter';
 END IF;
END $$;
SELECT 'schema: current install, independent PostGIS and Native namespaces, shadow tables and indexed reads passed';
