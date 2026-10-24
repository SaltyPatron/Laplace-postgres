\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
BEGIN;
CREATE TEMP TABLE projection_expected AS
SELECT laplace_id(word) AS id, laplace_tier(word) AS tier,
  laplace_coord(word) AS coord, laplace_hilbert(word) AS hilbert,
  laplace_path(ARRAY(SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table(word,'') c)) AS path
FROM unnest(ARRAY['dog','cat','ant']) word;
INSERT INTO projection_expected
SELECT laplace_compose(parts),2,point,
  laplace_hilbert4(point) # '-9223372036854775808'::bigint,laplace_path(parts)
FROM (SELECT ARRAY[laplace_id('dog'),laplace_id('dog'),laplace_id('cat')] AS parts,
  ST_GeomFromEWKB((SELECT laplace_centroid4d_ewkb(p) FROM unnest(ARRAY[
    laplace_coord('dog'),laplace_coord('dog'),laplace_coord('cat')]) p)) AS point) composed;
INSERT INTO public.entity(id,tier,coord,hilbert)
SELECT id,tier,CASE WHEN id=laplace_id('ant') THEN coord ELSE ST_MakePoint(0,0,0,0) END,
  CASE WHEN id=laplace_id('ant') THEN hilbert ELSE 0 END FROM projection_expected;
INSERT INTO public.physicality(entity,tier,hilbert,path,mask)
SELECT id,tier,CASE WHEN id=laplace_id('ant') THEN hilbert ELSE 0 END,path,
  laplace_mask_slot('kind','tuple') FROM projection_expected;
INSERT INTO public.witness(id,trust) SELECT id,0.5 FROM projection_expected WHERE tier=2;
INSERT INTO public.consensus(claim,rating,deviation,volatility,matches)
SELECT id,0.2,0.5,0.06,3 FROM projection_expected WHERE tier=2;
CREATE TEMP TABLE projection_before AS SELECT
  (SELECT jsonb_agg(to_jsonb(e) ORDER BY id) FROM public.entity e) AS entities,
  (SELECT jsonb_agg(to_jsonb(p) ORDER BY entity) FROM public.physicality p) AS paths,
  (SELECT jsonb_agg(to_jsonb(e)-'coord'-'hilbert' ORDER BY id) FROM public.entity e) AS entity_content,
  (SELECT jsonb_agg(to_jsonb(p)-'hilbert' ORDER BY entity) FROM public.physicality p) AS path_content,
  (SELECT jsonb_agg(to_jsonb(w) ORDER BY id) FROM public.witness w) AS witnesses,
  (SELECT jsonb_agg(to_jsonb(c) ORDER BY claim) FROM public.consensus c) AS consensus;
DO $$ DECLARE changes record; BEGIN
  IF NOT EXISTS(SELECT FROM projection_expected WHERE tier=2 AND ST_NPoints(path)=2 AND laplace_path_length(path)=3) THEN
    RAISE EXCEPTION 'fixture did not retain actual run-compressed path';
  END IF;
  SELECT * INTO changes FROM laplace_recoordinate();
  IF changes.entity_rows<>3 OR changes.physicality_rows<>3 THEN
    RAISE EXCEPTION 'audit did not propagate canonical corrections across tiers: %',changes;
  END IF;
  IF (SELECT entities FROM projection_before) IS DISTINCT FROM (SELECT jsonb_agg(to_jsonb(e) ORDER BY id) FROM public.entity e)
     OR (SELECT paths FROM projection_before) IS DISTINCT FROM (SELECT jsonb_agg(to_jsonb(p) ORDER BY entity) FROM public.physicality p) THEN
    RAISE EXCEPTION 'audit changed public rows';
  END IF;
  SELECT * INTO changes FROM laplace_recoordinate(true);
  IF changes.entity_rows<>3 OR changes.physicality_rows<>3 THEN RAISE EXCEPTION 'apply count differs from audit'; END IF;
  IF EXISTS(SELECT FROM projection_expected x JOIN public.entity e USING(id)
      JOIN public.physicality p ON p.entity=x.id WHERE ST_AsEWKB(e.coord)<>ST_AsEWKB(x.coord)
      OR e.hilbert<>x.hilbert OR p.hilbert<>x.hilbert OR ST_AsEWKB(p.path)<>ST_AsEWKB(x.path)
      OR p.mask<>laplace_mask_slot('kind','tuple') OR e.tier<>x.tier OR p.tier<>x.tier) THEN
    RAISE EXCEPTION 'apply changed identity/path/mask/tier or missed exact Native projection';
  END IF;
  IF (SELECT witnesses FROM projection_before) IS DISTINCT FROM (SELECT jsonb_agg(to_jsonb(w) ORDER BY id) FROM public.witness w)
     OR (SELECT consensus FROM projection_before) IS DISTINCT FROM (SELECT jsonb_agg(to_jsonb(c) ORDER BY claim) FROM public.consensus c) THEN
    RAISE EXCEPTION 'apply changed witnesses or consensus';
  END IF;
  IF (SELECT entity_content FROM projection_before) IS DISTINCT FROM (SELECT jsonb_agg(to_jsonb(e)-'coord'-'hilbert' ORDER BY id) FROM public.entity e)
     OR (SELECT path_content FROM projection_before) IS DISTINCT FROM (SELECT jsonb_agg(to_jsonb(p)-'hilbert' ORDER BY entity) FROM public.physicality p) THEN
    RAISE EXCEPTION 'apply changed immutable content or carrier metadata';
  END IF;
  SELECT * INTO changes FROM laplace_recoordinate();
  IF changes.entity_rows<>0 OR changes.physicality_rows<>0 THEN RAISE EXCEPTION 'repaired projection is not idempotent'; END IF;
END $$;
CREATE TEMP TABLE projection_repaired AS SELECT
  (SELECT jsonb_agg(to_jsonb(e) ORDER BY id) FROM public.entity e) AS entities,
  (SELECT jsonb_agg(to_jsonb(p) ORDER BY entity) FROM public.physicality p) AS paths;
DO $$ DECLARE missing blake3:=laplace_id('unrecorded projection child'); parent blake3; BEGIN
  BEGIN
    UPDATE public.physicality SET path=laplace_path(ARRAY[laplace_id('dog'),laplace_id('cat'),laplace_id('cat')]) WHERE tier=2;
    PERFORM laplace_recoordinate(true); RAISE EXCEPTION 'invalid stored identity accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
  BEGIN
    parent:=laplace_compose(ARRAY[laplace_id('dog'),missing]);
    INSERT INTO public.entity(id,tier,coord,hilbert) VALUES(parent,2,ST_MakePoint(0,0,0,0),0);
    INSERT INTO public.physicality(entity,tier,hilbert,path) VALUES(parent,2,0,laplace_path(ARRAY[laplace_id('dog'),missing]));
    PERFORM laplace_recoordinate(true); RAISE EXCEPTION 'missing child accepted';
  EXCEPTION WHEN foreign_key_violation THEN NULL; END;
  BEGIN
    UPDATE public.entity SET tier=2 WHERE id=laplace_id('dog');
    PERFORM laplace_recoordinate(true); RAISE EXCEPTION 'invalid topology accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
  BEGIN
    DELETE FROM public.physicality WHERE entity=laplace_id('dog');
    PERFORM laplace_recoordinate(true); RAISE EXCEPTION 'unpaired entity accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
  IF (SELECT entities FROM projection_repaired) IS DISTINCT FROM (SELECT jsonb_agg(to_jsonb(e) ORDER BY id) FROM public.entity e)
     OR (SELECT paths FROM projection_repaired) IS DISTINCT FROM (SELECT jsonb_agg(to_jsonb(p) ORDER BY entity) FROM public.physicality p) THEN
    RAISE EXCEPTION 'failed repair did not roll back completely';
  END IF;
  BEGIN
    PERFORM laplace_project_path(laplace_compose(ARRAY[laplace_id('dog'),laplace_id('cat')]),
      laplace_path(ARRAY[laplace_id('dog'),laplace_id('cat')]),ARRAY[laplace_id('dog'),laplace_id('cat')],
      ARRAY[ST_MakePoint(2,0,0,0),laplace_coord('cat')]);
    RAISE EXCEPTION 'out-of-wall child accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
  BEGIN
    PERFORM laplace_project_path(laplace_compose(ARRAY[laplace_id('dog'),laplace_id('cat')]),
      laplace_path(ARRAY[laplace_id('dog'),laplace_id('cat')]),ARRAY[laplace_id('dog'),laplace_id('cat')],
      ARRAY[ST_MakePoint(0.1,0,0,0),laplace_coord('cat')]);
    RAISE EXCEPTION 'off-grid child accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
  BEGIN
    PERFORM laplace_project_path(laplace_id('dog'),laplace_path(ARRAY[laplace_cp_id(100),laplace_cp_id(111),laplace_cp_id(103)]),
      ARRAY[laplace_cp_id(100),laplace_cp_id(100)],ARRAY[NULL::geometry,NULL::geometry]);
    RAISE EXCEPTION 'duplicate child bindings accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
ROLLBACK;
SELECT 'projection: Native run-weighted identity, canonical floor, audit, atomic repair, preservation and rollback passed';
