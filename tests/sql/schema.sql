\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
-- The deployed installation's bindings, read through its own namespace with a
-- caller search_path that names none of it. The database is shared: the
-- fixture is unique to this case and every statement is rolled back.
BEGIN;
CREATE FUNCTION pg_temp.leaf(n integer) RETURNS public.blake3 LANGUAGE sql IMMUTABLE
RETURN pg_catalog.md5('Laplace check regression schema leaf '||n)::public.blake3;
-- word: a check-unique word of real codepoints. pair: two leaves no other
-- content holds. said: the word followed by 'b', a record whose fact is 'b'.
INSERT INTO public.entity(id,tier,coord,hilbert)
SELECT public.laplace_id('lpcheckschema'),public.laplace_tier('lpcheckschema'),
       public.laplace_coord('lpcheckschema'),public.laplace_hilbert('lpcheckschema');
INSERT INTO public.physicality(entity,tier,hilbert,path)
SELECT id,tier,hilbert,public.laplace_path(ARRAY(
  SELECT public.laplace_cp_id(ascii(c)) FROM regexp_split_to_table('lpcheckschema','') c))
FROM public.entity WHERE id=public.laplace_id('lpcheckschema');
INSERT INTO public.entity(id,tier,coord,hilbert)
SELECT public.laplace_compose(ARRAY[pg_temp.leaf(1),pg_temp.leaf(2)]),1,
       public.laplace_coord('ab'),public.laplace_hilbert('ab');
INSERT INTO public.physicality(entity,tier,hilbert,path)
SELECT id,tier,hilbert,public.laplace_path(ARRAY[pg_temp.leaf(1),pg_temp.leaf(2)])
FROM public.entity WHERE id=public.laplace_compose(ARRAY[pg_temp.leaf(1),pg_temp.leaf(2)]);
INSERT INTO public.entity(id,tier,coord,hilbert)
SELECT public.laplace_compose(ARRAY[e.id,public.laplace_cp_id(98)]),e.tier+1,e.coord,e.hilbert
FROM public.entity e WHERE e.id=public.laplace_id('lpcheckschema');
INSERT INTO public.physicality(entity,tier,hilbert,path)
SELECT public.laplace_compose(ARRAY[e.id,public.laplace_cp_id(98)]),e.tier+1,e.hilbert,
       public.laplace_path(ARRAY[e.id,public.laplace_cp_id(98)])
FROM public.entity e WHERE e.id=public.laplace_id('lpcheckschema');
SET LOCAL search_path=pg_catalog,pg_temp;
CREATE DOMAIN pg_temp.test_blake3 AS public.blake3;
DO $$ DECLARE id pg_temp.test_blake3:=public.laplace_id('lpcheckschema');
  pair public.blake3:=public.laplace_compose(ARRAY[pg_temp.leaf(1),pg_temp.leaf(2)]); value text;
BEGIN
 IF public.laplace_text(id)<>'lpcheckschema' THEN RAISE EXCEPTION 'installed Native text binding failed'; END IF;
 IF public.laplace_path_length(public.laplace_path(ARRAY[id,id]))<>2 THEN
  RAISE EXCEPTION 'installed PostGIS function binding failed';
 END IF;
 IF pg_catalog.cardinality(public.laplace_vertex_ids(public.laplace_path(ARRAY[id,id])))<>1 THEN
  RAISE EXCEPTION 'installed PostGIS exporter binding failed';
 END IF;
 SELECT min(content) INTO STRICT value FROM public.laplace_text_batch(ARRAY[id,id]);
 IF value<>'lpcheckschema' THEN RAISE EXCEPTION 'batch text changed'; END IF;
 IF pg_catalog.encode(public.laplace_resolve_ref('lpcheckschema'),'hex')<>id::text
    OR pg_catalog.encode(public.laplace_resolve_ref(pg_catalog.upper(id::text)),'hex')<>id::text THEN
  RAISE EXCEPTION 'installed reference identity binding failed';
 END IF;
 IF (SELECT pg_catalog.array_agg(r.label ORDER BY r.ordinal)
     FROM public.laplace_resolve_refs(ARRAY[pg_catalog.decode(id::text,'hex'),pg_catalog.decode(id::text,'hex')],ARRAY[NULL,'caller surface']) r)
    IS DISTINCT FROM ARRAY['lpcheckschema','caller surface'] THEN
  RAISE EXCEPTION 'installed reference batch binding failed';
 END IF;
 IF (SELECT count(*) FROM public.laplace_paths(ARRAY[id]))<>1
    OR (SELECT count(*) FROM public.laplace_present_ids(ARRAY[id,id]))<>2 THEN
  RAISE EXCEPTION 'installed set binding failed';
 END IF;
 IF (SELECT pg_catalog.array_agg(ordinal ORDER BY ordinal)
       FROM public.laplace_entities(ARRAY[id,id])) IS DISTINCT FROM ARRAY[1,2]::bigint[]
    OR (SELECT count(*) FROM public.laplace_physicalities(ARRAY[id,id]))<>2 THEN
  RAISE EXCEPTION 'installed stored-row namespace or ordinal binding failed';
 END IF;
 IF (SELECT count(*) FROM public.laplace_containers(ARRAY[pg_temp.leaf(1)],'{}'::smallint[]))<>1 THEN
  RAISE EXCEPTION 'installed Native all-member binding lost its virtual basis';
 END IF;
 IF (SELECT count(*) FROM public.laplace_holders(ARRAY[pg_temp.leaf(1)],'{}',true))<>1
    OR (SELECT count(*) FROM public.laplace_holders(ARRAY[pg_temp.leaf(1)],'{}',true,-1::smallint,0))<>1
    OR EXISTS(SELECT FROM public.laplace_standings(ARRAY[id,id])) THEN
  RAISE EXCEPTION 'installed Native holders/standings binding changed';
 END IF;
 INSERT INTO public.attestation(claim,witness,lineage,games,tokens,score,"position",trust)
   VALUES(pg_catalog.md5('schema-witness-'||pair::text)::public.blake3,pair,NULL,1,1,1,NULL,1);
 -- Large-set GIN membership in the backend that wrote the rows.
 UPDATE public.physicality SET path=public.ST_SetPoint(path,0,
   public.ST_MakePoint(
     public.ST_X(public.ST_PointN(path,1)),
     public.ST_Y(public.ST_PointN(path,1)),
     public.ST_Z(public.ST_PointN(path,1)),
     public.ST_M(public.ST_PointN(path,1))+1073741824))
 WHERE entity OPERATOR(public.=) pair;
 IF (SELECT count(*) FROM public.laplace_attested(
       ARRAY[pg_temp.leaf(1)] ||
       ARRAY(SELECT pg_catalog.md5(i::text)::public.blake3 FROM pg_catalog.generate_series(1,600) i))
       WHERE trunk OPERATOR(public.=) pair AND games=1 AND witness)<>1 THEN
  RAISE EXCEPTION 'installed exact GIN set membership failed';
 END IF;
 IF (SELECT count(*) FROM public.laplace_ordered_pair_counts(NULL,1,1,ARRAY[pair]))<>1
    OR pg_catalog.cardinality(public.laplace_separator_ids())<>85 THEN
  RAISE EXCEPTION 'installed ordered structure binding failed';
 END IF;
 IF EXISTS(SELECT FROM pg_proc p JOIN pg_depend d ON d.classid='pg_proc'::regclass AND d.objid=p.oid
  JOIN pg_extension e ON e.oid=d.refobjid AND d.refclassid='pg_extension'::regclass
  WHERE e.extname='laplace' AND d.deptype='e' AND NOT p.prosecdef AND p.proconfig IS NOT NULL
    AND NOT (p.proname='laplace_merge_stage' AND p.pronargs=3
      AND p.proconfig=ARRAY['plan_cache_mode=force_generic_plan'])) THEN
  RAISE EXCEPTION 'ordinary Native or SQL function retains per-call settings';
 END IF;
END $$;
-- The same installed-object boundary covers set publication, including ID
-- equality inside dynamic SQL. The stage tables are this case's own.
CREATE UNLOGGED TABLE stage.check_schema_entities (LIKE stage.entity INCLUDING DEFAULTS);
CREATE UNLOGGED TABLE stage.check_schema_paths (LIKE stage.physicality INCLUDING DEFAULTS);
CREATE UNLOGGED TABLE stage.check_schema_attestation (LIKE stage.attestation INCLUDING DEFAULTS INCLUDING CONSTRAINTS);
INSERT INTO stage.check_schema_entities SELECT * FROM public.entity
  WHERE id OPERATOR(public.=) ANY(ARRAY[public.laplace_id('lpcheckschema'),public.laplace_compose(ARRAY[pg_temp.leaf(1),pg_temp.leaf(2)])]);
INSERT INTO stage.check_schema_paths SELECT * FROM public.physicality
  WHERE entity OPERATOR(public.=) ANY(ARRAY[public.laplace_id('lpcheckschema'),public.laplace_compose(ARRAY[pg_temp.leaf(1),pg_temp.leaf(2)])]);
DO $$ DECLARE merged record; said public.blake3:=public.laplace_compose(ARRAY[public.laplace_id('lpcheckschema'),public.laplace_cp_id(98)]); BEGIN
 SELECT * INTO merged FROM public.laplace_merge_stage('stage.check_schema_entities','stage.check_schema_paths');
 IF merged.entity_rows<>0 OR merged.physicality_rows<>0 THEN RAISE EXCEPTION 'namespace replay inserted duplicate content'; END IF;
 UPDATE public.physicality SET mask=public.laplace_mask_slot('kind','claim') WHERE entity OPERATOR(public.=) said;
 IF (SELECT fact FROM public.laplace_claim_preview(ARRAY[said]))<>'b' THEN
  RAISE EXCEPTION 'namespace claim preview changed selected content';
 END IF;
 IF public.laplace_attest_series('stage.check_schema_attestation',ARRAY[public.laplace_id('lpcheckschema')])<>0 THEN
  RAISE EXCEPTION 'namespace publication created evidence';
 END IF;
END $$;
ROLLBACK;
BEGIN;
DO $$ BEGIN
 IF public.laplace_path_length(public.laplace_path(ARRAY[public.laplace_cp_id(97),public.laplace_cp_id(98)]))<>2 THEN
  RAISE EXCEPTION 'rolled-back fixture poisoned a cached PostGIS binding';
 END IF;
 IF pg_catalog.cardinality(public.laplace_vertex_ids(public.laplace_path(ARRAY[public.laplace_cp_id(97),public.laplace_cp_id(98)])))<>2 THEN
  RAISE EXCEPTION 'rolled-back fixture poisoned a cached PostGIS exporter';
 END IF;
END $$;
ROLLBACK;
SELECT 'schema: installed namespace bindings, caller search_path independence, shadow-free publication and indexed reads passed';
