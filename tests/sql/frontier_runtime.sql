\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
BEGIN;
-- Exercise the complete Native traversal across the real partition layout,
-- rather than timing only one geometry predicate. Every stored word is
-- canonical Native content; the missing IDs are lookup keys, not entities.
CREATE TEMP TABLE frontier_words AS
SELECT 'frontier' || chr(97 + i % 26) || chr(97 + (i / 26) % 26) || chr(97 + (i / 676) % 26) AS content
FROM generate_series(0,16999) i;
INSERT INTO public.entity(id,tier,coord,hilbert)
SELECT laplace_id(content),laplace_tier(content),laplace_coord(content),laplace_hilbert(content)
FROM frontier_words;
INSERT INTO public.physicality(entity,tier,hilbert,path)
SELECT laplace_id(content),laplace_tier(content),laplace_hilbert(content),
       laplace_path(ARRAY(SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table(content,'') c))
FROM frontier_words;
ANALYZE public.physicality;
SET LOCAL work_mem='16MB';
DO $$
DECLARE found bigint;
BEGIN
  SELECT count(*) INTO found FROM laplace_attribution(
    ARRAY(SELECT md5(i::text)::blake3 FROM generate_series(100001,220000) i));
  IF found <> 0 THEN RAISE EXCEPTION 'missing frontier acquired witness evidence'; END IF;
END $$;
-- A dense frontier must also retain genuine matches across multiple transfer
-- windows. Annotate the existing canonical first scalar as an assertion.
UPDATE public.physicality SET path=ST_SetPoint(path,0,ST_MakePoint(
 ST_X(ST_PointN(path,1)),ST_Y(ST_PointN(path,1)),ST_Z(ST_PointN(path,1)),
 ST_M(ST_PointN(path,1))+1073741824));
SET LOCAL work_mem='64kB';
DO $$ DECLARE found bigint; BEGIN
 SELECT count(*) INTO found FROM laplace_attested(
  ARRAY[laplace_cp_id(102)] || ARRAY(SELECT md5(i::text)::blake3 FROM generate_series(100001,105000) i));
 IF found<>17000 THEN RAISE EXCEPTION 'dense frontier lost true matches or repeated a transfer window: %',found; END IF;
END $$;
-- One real parent introduces a large member dictionary. Sparse exact seeks
-- must union matches across every transfer window and emit this parent once.
-- Build its ID, coordinate and Hilbert key through the Native composition API.
DO $$
DECLARE ids blake3[]; coords geometry[]; parent blake3; path geometry;
        projection record; child_tier smallint; index_id oid; found bigint; memory text; endpoint smallint;
BEGIN
 SELECT array_agg(e.id ORDER BY e.id),array_agg(e.coord ORDER BY e.id),max(e.tier)
 INTO ids,coords,child_tier FROM frontier_words w JOIN public.entity e ON e.id=laplace_id(w.content);
 parent:=laplace_compose(ids); path:=laplace_path(ids);
 path:=ST_SetPoint(path,0,ST_MakePoint(ST_X(ST_PointN(path,1)),ST_Y(ST_PointN(path,1)),
   ST_Z(ST_PointN(path,1)),ST_M(ST_PointN(path,1))+1073741824));
 SELECT * INTO projection FROM laplace_project_path(parent,path,ids,coords);
 INSERT INTO public.entity(id,tier,coord,hilbert)
 VALUES(parent,child_tier+1,ST_GeomFromEWKB(projection.coord_ewkb),projection.hilbert);
 INSERT INTO public.physicality(entity,tier,hilbert,path,mask)
 VALUES(parent,child_tier+1,projection.hilbert,path,laplace_mask_slot('kind','claim'));
 INSERT INTO public.consensus
 SELECT parent,s.rating,s.deviation,s.volatility,s.matches FROM laplace_series(
   ARRAY[laplace_cp_id(102)],ARRAY[1::bigint],ARRAY[1::float8],ARRAY[1::float8]) s;
 FOR index_id IN SELECT i.indexrelid FROM pg_catalog.pg_index i
   JOIN pg_catalog.pg_class c ON c.oid=i.indexrelid
   JOIN pg_catalog.pg_am a ON a.oid=c.relam
   WHERE i.indrelid=(SELECT tableoid FROM public.physicality WHERE entity=parent) AND a.amname='gin'
 LOOP PERFORM public.laplace_clean_pending_list(index_id::regclass); END LOOP;
 FOREACH memory IN ARRAY ARRAY['64kB','16MB'] LOOP
   PERFORM set_config('work_mem',memory,true);
   SELECT count(*) INTO found FROM public.laplace_holders(
     ids[1:513] || ARRAY(SELECT md5(i::text)::blake3 FROM generate_series(100001,100487) i)) h
   WHERE h.entity=parent;
   IF found<>1 THEN RAISE EXCEPTION 'sparse frontier lost or repeated its canonical parent under %: %',memory,found; END IF;
   FOR endpoint IN 1..3 LOOP
     SELECT count(*) INTO found FROM public.laplace_claims_each(
       ids[1:513] || ids[17000:17000] || ARRAY(SELECT md5(i::text)::blake3 FROM generate_series(100001,100487) i),
       NULL::bigint,'{}'::smallint[],'{}'::blake3[],endpoint::smallint) c WHERE c.entity=parent;
     IF found<>(CASE WHEN endpoint=3 THEN 2 ELSE 1 END) THEN
       RAISE EXCEPTION 'endpoint set changed input ordinals under %, direction %: %',memory,endpoint,found;
     END IF;
   END LOOP;
 END LOOP;
END $$;
ROLLBACK;
SELECT 'frontier runtime: complete traversal over 17000 canonical paths passed';
