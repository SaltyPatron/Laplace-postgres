\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
BEGIN;
SELECT laplace_ensure_stage();
CREATE UNLOGGED TABLE stage.annotation_entities (LIKE stage.entity INCLUDING DEFAULTS);
CREATE UNLOGGED TABLE stage.annotation_paths (LIKE stage.physicality INCLUDING DEFAULTS);
CREATE TEMP TABLE annotation_fixture AS
WITH parts AS (
  SELECT unnest(ARRAY['0','01','01']) AS text
), value AS (
  SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(laplace_coord(text))) AS coord FROM parts
)
SELECT laplace_compose(ARRAY[laplace_id('0'),laplace_id('01'),laplace_id('01')]) AS id,
       coord, laplace_hilbert4(coord) AS hilbert,
       laplace_path(ARRAY[laplace_id('0'),laplace_id('01'),laplace_id('01')]) AS path
FROM value;
CREATE FUNCTION pg_temp.annotation_batch() RETURNS void LANGUAGE sql AS $$
  INSERT INTO stage.annotation_entities(id,tier,coord,hilbert) SELECT id,2,coord,hilbert FROM annotation_fixture;
  INSERT INTO stage.annotation_paths(entity,tier,hilbert,path) SELECT id,2,hilbert,path FROM annotation_fixture;
$$;
SELECT pg_temp.annotation_batch();
DO $$
DECLARE inserted record; changed record; old_path geometry; known_path geometry; rejected boolean=false;
BEGIN
  SELECT * INTO inserted FROM laplace_merge_stage('stage.annotation_entities','stage.annotation_paths');
  IF inserted.entity_rows<>1 OR inserted.physicality_rows<>1 THEN RAISE EXCEPTION 'initial path was not inserted'; END IF;
  SELECT path INTO old_path FROM annotation_fixture;
  known_path=ST_SetPoint(old_path,ST_NPoints(old_path)-1,
    ST_MakePoint(ST_X(ST_EndPoint(old_path)),ST_Y(ST_EndPoint(old_path)),ST_Z(ST_EndPoint(old_path)),ST_M(ST_EndPoint(old_path))+3221225472));
  PERFORM pg_temp.annotation_batch();
  UPDATE stage.annotation_paths SET path=known_path;
  SELECT * INTO changed FROM laplace_merge_stage('stage.annotation_entities','stage.annotation_paths');
  IF changed.entity_rows<>0 OR changed.physicality_rows<>0 OR changed.updated_rows<>1
     OR NOT EXISTS (SELECT FROM public.physicality p JOIN annotation_fixture f ON p.entity=f.id
                    WHERE ST_AsEWKB(p.path)=ST_AsEWKB(known_path) AND laplace_path_length(p.path)=3) THEN
    RAISE EXCEPTION 'known annotation did not enrich the existing canonical path';
  END IF;
  PERFORM pg_temp.annotation_batch();
  SELECT * INTO changed FROM laplace_merge_stage('stage.annotation_entities','stage.annotation_paths');
  IF changed.updated_rows<>0 OR NOT EXISTS (SELECT FROM public.physicality p JOIN annotation_fixture f ON p.entity=f.id
                    WHERE ST_AsEWKB(p.path)=ST_AsEWKB(known_path)) THEN RAISE EXCEPTION 'replay erased the known annotation'; END IF;
  PERFORM pg_temp.annotation_batch();
  UPDATE stage.annotation_paths SET path=ST_SetPoint(old_path,ST_NPoints(old_path)-1,
    ST_MakePoint(ST_X(ST_EndPoint(old_path)),ST_Y(ST_EndPoint(old_path)),ST_Z(ST_EndPoint(old_path)),ST_M(ST_EndPoint(old_path))+2147483648));
  BEGIN
    PERFORM laplace_merge_stage('stage.annotation_entities','stage.annotation_paths');
  EXCEPTION WHEN check_violation THEN rejected=true;
  END;
  IF NOT rejected OR (SELECT count(*) FROM stage.annotation_paths)<>1
     OR NOT EXISTS (SELECT FROM public.physicality p JOIN annotation_fixture f ON p.entity=f.id
                    WHERE ST_AsEWKB(p.path)=ST_AsEWKB(known_path)) THEN
    RAISE EXCEPTION 'conflicting annotation was accepted or consumed copied input';
  END IF;
END $$;
ROLLBACK;
