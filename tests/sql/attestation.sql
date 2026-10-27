\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
BEGIN;
CREATE SCHEMA IF NOT EXISTS stage;
CREATE UNLOGGED TABLE stage.attestation_test (
  claim blake3 NOT NULL, witness blake3 NOT NULL, lineage blake3,
  games bigint NOT NULL, score double precision NOT NULL, trust double precision NOT NULL,
  PRIMARY KEY (claim, witness)
);

-- Real canonical content, coordinates and containment paths. Tier-0 atoms
-- remain in the mapped cache; no codepoint rows are inserted in the database.
CREATE TEMP TABLE series_words AS SELECT unnest(ARRAY['dog','is','noun','source','file','one','two','three']) AS content;
INSERT INTO entity (id, tier, coord, hilbert)
  SELECT laplace_id(content), laplace_tier(content), laplace_coord(content), laplace_hilbert(content) FROM series_words;
INSERT INTO physicality (entity, tier, hilbert, path)
  SELECT laplace_id(content), laplace_tier(content), laplace_hilbert(content),
    laplace_path(ARRAY(SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table(content, '') c)) FROM series_words;
CREATE FUNCTION pg_temp.series_node(parts blake3[], kind text DEFAULT NULL, metadata bigint[] DEFAULT NULL) RETURNS blake3
LANGUAGE plpgsql AS $$
DECLARE composed blake3 := laplace_compose(parts); point geometry; path geometry := laplace_path(parts);
        coord geometry; altitude smallint; hilbert bigint; carrier bit(64) := B'0'::bit(64); i integer;
BEGIN
  SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(
           coalesce(e.coord,ST_GeomFromEWKB(laplace_cp_coord_ewkb(laplace_codepoint(p)))))),
         (max(coalesce(e.tier,0)) + 1)::smallint
    INTO coord, altitude FROM unnest(parts) p LEFT JOIN entity e ON e.id = p;
  hilbert := laplace_hilbert4(coord) # '-9223372036854775808'::bigint;
  IF kind IS NOT NULL THEN carrier := laplace_mask_slot('kind', kind); END IF;
  IF metadata IS NOT NULL THEN
    IF cardinality(metadata) <> ST_NPoints(path) THEN RAISE EXCEPTION 'fixture metadata does not match RLE vertices'; END IF;
    FOR i IN 1..cardinality(metadata) LOOP
      point := ST_PointN(path, i);
      path := ST_SetPoint(path, i - 1, ST_MakePoint(ST_X(point), ST_Y(point), ST_Z(point), ST_M(point) + metadata[i]));
    END LOOP;
  END IF;
  INSERT INTO entity (id, tier, coord, hilbert, mask) VALUES (composed, altitude, coord, hilbert, carrier) ON CONFLICT DO NOTHING;
  INSERT INTO physicality (entity, tier, hilbert, path, mask) VALUES (composed, altitude, hilbert, path, carrier) ON CONFLICT DO NOTHING;
  RETURN composed;
END $$;
SAVEPOINT native_batch_realization;
DO $$
DECLARE deep blake3:=laplace_id('dog'); bang blake3:=laplace_cp_id(ascii('!'));
        lambda blake3:=laplace_cp_id(ascii('λ')); chinese blake3:=laplace_cp_id(ascii('中'));
        repeated blake3; tuple blake3; tuple_run blake3; nested_tuple blake3; package blake3;
        preview_claim blake3; preview_joint blake3; preview_binary blake3;
        expected text:='dog'||repeat('!',70); actual text[];
BEGIN
  -- Linear-size content with more than the former 64-level rejection: every
  -- identity, coordinate and packed path is made by the Native operators.
  FOR i IN 1..70 LOOP deep:=pg_temp.series_node(ARRAY[deep,bang]); END LOOP;
  repeated:=pg_temp.series_node(array_fill(laplace_id('dog'),ARRAY[3]));
  SELECT array_agg(content ORDER BY i) INTO actual FROM laplace_text_batch(ARRAY[lambda,deep,chinese,deep,repeated]);
  IF actual IS DISTINCT FROM ARRAY['λ',expected,'中',expected,'dogdogdog']
     OR laplace_text(deep)<>expected
     OR EXISTS(SELECT FROM laplace_text_batch(ARRAY[]::blake3[])) THEN
    RAISE EXCEPTION 'batch realization changed Unicode, runs, occurrences or deep Native content';
  END IF;
  tuple:=pg_temp.series_node(ARRAY[laplace_id('dog'),lambda,chinese],'tuple');
  tuple_run:=pg_temp.series_node(array_fill(laplace_id('dog'),ARRAY[2]),'tuple');
  nested_tuple:=pg_temp.series_node(ARRAY[tuple,tuple_run],'tuple');
  package:=pg_temp.series_node(ARRAY[laplace_id('file'),deep],'file');
  SELECT array_agg(content ORDER BY i) INTO actual
  FROM laplace_labels(ARRAY[tuple,tuple_run,nested_tuple,package,'00000000000000000000000000000000'::blake3,deep]);
  IF actual IS DISTINCT FROM ARRAY['[dog · λ · 中]','[dog × 2]','[[dog · λ · 中] · [dog × 2]]',
                                  package::text,'00000000000000000000000000000000',expected] THEN
    RAISE EXCEPTION 'Native labels changed tuple structure, runs or selected identities';
  END IF;
  preview_claim:=pg_temp.series_node(ARRAY[laplace_id('dog'),laplace_id('is'),laplace_id('noun')],'claim');
  preview_joint:=pg_temp.series_node(array_fill(laplace_id('dog'),ARRAY[4]),'claim');
  preview_binary:=pg_temp.series_node(ARRAY[laplace_id('dog'),laplace_id('noun')],'claim');
  SELECT array_agg(fact ORDER BY i) INTO actual FROM laplace_claim_preview(ARRAY[
    preview_claim,preview_joint,preview_binary,
    preview_claim,package,'00000000000000000000000000000000'::blake3]);
  IF actual IS DISTINCT FROM ARRAY['noun','dog × 4','noun','noun']
     OR EXISTS(SELECT FROM laplace_claim_preview(ARRAY[preview_claim])
       WHERE predicate IS DISTINCT FROM laplace_id('is') OR object IS DISTINCT FROM laplace_id('noun') OR type_label<>'is')
     OR EXISTS(SELECT FROM laplace_claim_preview(ARRAY[]::blake3[])) THEN
    RAISE EXCEPTION 'Native preview changed typed claims, n-ary/RLE content, input occurrences or missing paths: %',actual;
  END IF;
  SELECT array_agg(label ORDER BY ordinal) INTO actual FROM laplace_resolve_refs(
    ARRAY[decode(deep::text,'hex'),decode(tuple::text,'hex'),decode(deep::text,'hex'),
          decode(lambda::text,'hex'),decode('00000000000000000000000000000000','hex')],
    ARRAY[NULL,NULL,'caller surface',NULL,NULL]);
  IF actual IS DISTINCT FROM ARRAY[expected,'[dog · λ · 中]','caller surface','λ','00000000000000000000000000000000']
     OR (SELECT count(*) FROM laplace_resolve_refs(
          ARRAY[decode(deep::text,'hex'),decode(lambda::text,'hex'),decode('00000000000000000000000000000000','hex')],
          ARRAY[NULL,NULL,NULL]) WHERE present)<>2 THEN
    RAISE EXCEPTION 'Native references changed occurrences, surfaces, virtual floor or missing membership';
  END IF;
  IF encode(laplace_resolve_ref('dog'),'hex')<>laplace_id('dog')::text
     OR encode(laplace_resolve_ref(upper(deep::text)),'hex')<>deep::text
     OR encode(laplace_resolve_ref('λ'),'hex')<>lambda::text
     OR laplace_resolve_ref('00000000000000000000000000000000') IS NOT NULL
     OR laplace_resolve_ref('') IS NOT NULL OR laplace_resolve_ref(NULL) IS NOT NULL
     OR EXISTS(SELECT FROM laplace_resolve_refs(NULL,NULL))
     OR EXISTS(SELECT FROM laplace_resolve_refs(ARRAY[]::bytea[],ARRAY[]::text[])) THEN
    RAISE EXCEPTION 'Native scalar/empty reference contract changed';
  END IF;
  BEGIN
    PERFORM laplace_resolve_refs(ARRAY[decode(deep::text,'hex')],ARRAY[]::text[]);
    RAISE EXCEPTION 'mismatched reference positions accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    PERFORM laplace_resolve_refs(ARRAY[NULL]::bytea[],ARRAY[NULL]::text[]);
    RAISE EXCEPTION 'NULL reference identity accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    PERFORM laplace_resolve_refs(ARRAY[decode('0011','hex')],ARRAY[NULL]::text[]);
    RAISE EXCEPTION 'malformed reference identity accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    UPDATE physicality SET path=laplace_path(ARRAY[deep,bang]) WHERE entity=deep;
    PERFORM laplace_text_batch(ARRAY[deep]);
    RAISE EXCEPTION 'cyclic Native carrier accepted';
  EXCEPTION WHEN data_exception THEN NULL; END;
  IF laplace_text(deep)<>expected THEN RAISE EXCEPTION 'failed realization poisoned the next Native read'; END IF;
END $$;
ROLLBACK TO native_batch_realization;
RELEASE native_batch_realization;
CREATE TEMP TABLE series_claims (name text PRIMARY KEY, id blake3);
INSERT INTO series_claims VALUES
  ('a', pg_temp.series_node(ARRAY[laplace_id('dog'),laplace_id('is'),laplace_id('noun')], 'claim')),
  ('b', pg_temp.series_node(ARRAY[laplace_id('noun'),laplace_id('is'),laplace_id('dog')], 'claim'));
CREATE TEMP TABLE series_sources (name text PRIMARY KEY, id blake3);
DO $$
DECLARE source_name text; a blake3 := (SELECT id FROM series_claims WHERE name = 'a');
        b blake3 := (SELECT id FROM series_claims WHERE name = 'b'); rec blake3; file_meta blake3; file_trunk blake3; source_meta blake3;
BEGIN
  FOREACH source_name IN ARRAY ARRAY['one','two','three'] LOOP
    -- A record says claim a 1000 times and claim b 3 times; both counts are
    -- real RLE runs in M, above which file and source trunks contain them.
    rec := pg_temp.series_node(array_fill(a, ARRAY[1000]) || array_fill(b, ARRAY[3]) || ARRAY[laplace_id(source_name)], 'record',
                              ARRAY[2147483648,2147483648,0]::bigint[]);
    file_meta := pg_temp.series_node(ARRAY[laplace_id('file'),laplace_id(source_name)], 'tuple');
    file_trunk := pg_temp.series_node(ARRAY[file_meta,rec], 'file', ARRAY[4294967296,5368709120]::bigint[]);
    source_meta := pg_temp.series_node(ARRAY[laplace_id('source'),laplace_id(source_name)], 'tuple');
    INSERT INTO series_sources VALUES (source_name, pg_temp.series_node(ARRAY[source_meta,file_trunk], 'file', ARRAY[4294967296,5368709120]::bigint[]));
  END LOOP;
END $$;
-- The same prepared topology serves independent roots and a root containing
-- another requested root. Zero-count branches must not affect the latter's
-- own series, and batching/reordering roots cannot change any testimony field.
SAVEPOINT shared_downward_topology;
DO $$
DECLARE a blake3 := (SELECT id FROM series_claims WHERE name='a');
        one_root blake3 := (SELECT id FROM series_sources WHERE name='one');
        two_root blake3 := (SELECT id FROM series_sources WHERE name='two');
        only_a blake3; nested blake3; roots blake3[];
BEGIN
  only_a := pg_temp.series_node(array_fill(a,ARRAY[7]) || ARRAY[laplace_id('three')],
      'record',ARRAY[2147483648,0]::bigint[]);
  nested := pg_temp.series_node(ARRAY[one_root,only_a],
      'file',ARRAY[5368709120,5368709120]::bigint[]);
  roots := ARRAY[two_root,nested,only_a,one_root,two_root];
  IF EXISTS (
    (SELECT * FROM laplace_contained_series(roots)
     EXCEPT ALL
     SELECT s.* FROM unnest(ARRAY[one_root,two_root,nested,only_a]) r
       CROSS JOIN LATERAL laplace_contained_series(ARRAY[r]) s)
    UNION ALL
    (SELECT s.* FROM unnest(ARRAY[one_root,two_root,nested,only_a]) r
       CROSS JOIN LATERAL laplace_contained_series(ARRAY[r]) s
     EXCEPT ALL
     SELECT * FROM laplace_contained_series(roots))
  ) THEN RAISE EXCEPTION 'shared downward topology changed root-local testimony'; END IF;
  IF (SELECT games FROM laplace_contained_series(roots) WHERE trunk=nested AND claim=a)<>1007
     OR (SELECT games FROM laplace_contained_series(roots) WHERE trunk=one_root AND claim=a)<>1000
     OR (SELECT games FROM laplace_contained_series(roots) WHERE trunk=only_a AND claim=a)<>7
     OR (SELECT count(*) FROM laplace_contained_series(roots))<>7 THEN
    RAISE EXCEPTION 'nested or disjoint root counts changed';
  END IF;
END $$;
ROLLBACK TO shared_downward_topology;
RELEASE shared_downward_topology;
INSERT INTO stage.attestation_test SELECT c.id, w.id, NULL, 1000, 1, 0.85
  FROM series_claims c, series_sources w WHERE c.name = 'a' AND w.name = 'one';
DO $$
DECLARE trunks blake3[] := ARRAY(SELECT id FROM series_sources ORDER BY id);
        claims blake3[] := ARRAY(SELECT id FROM series_claims ORDER BY id); found integer;
BEGIN
  SELECT count(*) INTO found FROM laplace_contained_series(trunks);
  IF found<>6 THEN RAISE EXCEPTION 'downward Native series lost a source or claim'; END IF;
  IF EXISTS (
    (SELECT claim,trunk,games,tokens,score,"position",dependence_root FROM laplace_contained_series(trunks)
     EXCEPT ALL
     SELECT claim,trunk,games,tokens,score,"position",dependence_root FROM laplace_attested_series(claims)
     WHERE trunk=ANY(trunks))
    UNION ALL
    (SELECT claim,trunk,games,tokens,score,"position",dependence_root FROM laplace_attested_series(claims)
     WHERE trunk=ANY(trunks)
     EXCEPT ALL
     SELECT claim,trunk,games,tokens,score,"position",dependence_root FROM laplace_contained_series(trunks))
  ) THEN RAISE EXCEPTION 'downward series differs from upward Native containment'; END IF;
  IF EXISTS (
    (SELECT * FROM laplace_contained_series(trunks,ARRAY[claims[1],claims[1]])
     EXCEPT ALL
     SELECT * FROM laplace_contained_series(trunks) WHERE claim=claims[1])
    UNION ALL
    (SELECT * FROM laplace_contained_series(trunks) WHERE claim=claims[1]
     EXCEPT ALL
     SELECT * FROM laplace_contained_series(trunks,ARRAY[claims[1],claims[1]]))
  ) OR (SELECT count(*) FROM laplace_contained_series(trunks,ARRAY[claims[1]]))<>3 THEN
    RAISE EXCEPTION 'selected downward series changed testimony or returned unrelated claims';
  END IF;
  IF EXISTS(SELECT FROM laplace_contained_series(trunks,ARRAY[]::blake3[]))
     OR EXISTS(SELECT FROM laplace_contained_series(ARRAY[]::blake3[],claims))
     OR EXISTS(SELECT FROM laplace_contained_series(trunks,ARRAY[laplace_id('dog')])) THEN
    RAISE EXCEPTION 'empty or unattested claim selection returned testimony';
  END IF;
  IF (SELECT count(*) FROM laplace_contained_series(ARRAY[trunks[1],trunks[1]]))<>2
     OR EXISTS(SELECT FROM laplace_contained_series(ARRAY[]::blake3[])) THEN
    RAISE EXCEPTION 'duplicate or empty trunk selection changed Native series';
  END IF;
  BEGIN
    PERFORM laplace_contained_series(ARRAY['00000000000000000000000000000000'::blake3]);
    RAISE EXCEPTION 'absent stored root accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    PERFORM laplace_contained_series(ARRAY['00000000000000000000000000000000'::blake3],claims);
    RAISE EXCEPTION 'selected series accepted an absent stored root';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
-- An unpublished source may already have committed structural containers.
-- Those containers are not independent historical witnesses for its pending
-- observations, however deep their packaging becomes.
DO $$
DECLARE a blake3 := (SELECT id FROM series_claims WHERE name='a');
        orphan blake3; before_row record; after_row record;
BEGIN
  SELECT * INTO before_row FROM laplace_series_snapshot('stage.attestation_test',ARRAY[a]);
  orphan := pg_temp.series_node(ARRAY[a,laplace_id('file')],'record',ARRAY[1073741824,0]::bigint[]);
  FOR i IN 1..80 LOOP
    orphan := pg_temp.series_node(ARRAY[laplace_id('file'),orphan],'file',ARRAY[4294967296,0]::bigint[]);
  END LOOP;
  SELECT * INTO after_row FROM laplace_series_snapshot('stage.attestation_test',ARRAY[a]);
  IF before_row IS DISTINCT FROM after_row THEN
    RAISE EXCEPTION 'unpublished source containers changed historical evidence';
  END IF;
END $$;
-- Private computation shares the real Native solver without publishing an
-- unregistered source or retaining a consensus bucket lock during streaming.
DO $$
DECLARE a blake3 := (SELECT id FROM series_claims WHERE name = 'a');
        b blake3 := (SELECT id FROM series_claims WHERE name = 'b');
        snapshot record; expected record; before_rows text; after_rows text;
        before_locks bigint; after_locks bigint;
BEGIN
  SELECT md5(coalesce(string_agg(c::text, E'\n' ORDER BY c.claim), '')) INTO before_rows FROM consensus c;
  SELECT count(*) INTO before_locks FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'advisory';
  SELECT * INTO snapshot FROM laplace_series_snapshot('stage.attestation_test', ARRAY[a, a]);
  SELECT s.* INTO expected FROM stage.attestation_test p,
    LATERAL laplace_series(ARRAY[p.witness], ARRAY[p.games], ARRAY[p.score], ARRAY[p.trust]) s;
  IF (snapshot.rating, snapshot.deviation, snapshot.volatility, snapshot.matches) IS DISTINCT FROM
     (expected.rating, expected.deviation, expected.volatility, expected.matches) THEN
    RAISE EXCEPTION 'private snapshot differs from Native standing';
  END IF;
  SELECT md5(coalesce(string_agg(c::text, E'\n' ORDER BY c.claim), '')) INTO after_rows FROM consensus c;
  SELECT count(*) INTO after_locks FROM pg_locks WHERE pid = pg_backend_pid() AND locktype = 'advisory';
  IF before_rows <> after_rows OR before_locks <> after_locks THEN
    RAISE EXCEPTION 'private snapshot changed public state or acquired a consensus lock';
  END IF;
  IF (SELECT count(*) FROM laplace_series_snapshot('stage.attestation_test', ARRAY[a, a])) <> 1
     OR EXISTS (SELECT FROM laplace_series_snapshot('stage.attestation_test', ARRAY[]::blake3[])) THEN
    RAISE EXCEPTION 'snapshot duplicate/empty selection changed';
  END IF;
  SELECT * INTO snapshot FROM laplace_series_snapshot('stage.attestation_test', ARRAY[b]);
  IF snapshot.claim IS DISTINCT FROM b OR snapshot.rating IS NOT NULL OR snapshot.matches IS NOT NULL THEN
    RAISE EXCEPTION 'snapshot did not preserve an evidence-free removal candidate';
  END IF;
  BEGIN
    PERFORM laplace_series_snapshot('public.consensus');
    RAISE EXCEPTION 'snapshot accepted a public pending table';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    PERFORM laplace_series_snapshot('stage.attestation_test', ARRAY[laplace_id('dog')]);
    RAISE EXCEPTION 'snapshot accepted a non-claim';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
-- The stage-resident batch and the array adapter must share one Native
-- calculation, including duplicate selections and evidence-free removals.
CREATE UNLOGGED TABLE stage.attestation_selected (claim blake3);
INSERT INTO stage.attestation_selected SELECT id FROM series_claims;
INSERT INTO stage.attestation_selected SELECT id FROM series_claims WHERE name='a';
CREATE TEMP TABLE series_selected AS TABLE stage.attestation_selected;
DO $$
DECLARE wanted blake3[] := ARRAY(SELECT claim FROM stage.attestation_selected);
BEGIN
  IF EXISTS (
    (SELECT * FROM laplace_stage_snapshot('stage.attestation_test','stage.attestation_selected')
     EXCEPT ALL SELECT * FROM laplace_series_snapshot('stage.attestation_test',wanted))
    UNION ALL
    (SELECT * FROM laplace_series_snapshot('stage.attestation_test',wanted)
     EXCEPT ALL SELECT * FROM laplace_stage_snapshot('stage.attestation_test','series_selected'))
  ) THEN RAISE EXCEPTION 'stage and array selections disagree'; END IF;
  IF laplace_attest_stage('stage.attestation_test','stage.attestation_selected') <> 1
     OR laplace_attest_stage('stage.attestation_test','series_selected') <> 1 THEN
    RAISE EXCEPTION 'stage publication lost cumulative replay or selection deduplication';
  END IF;
  BEGIN
    PERFORM laplace_stage_snapshot('stage.attestation_test','public.consensus');
    RAISE EXCEPTION 'public changed-claim table accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
DO $$
DECLARE a blake3 := (SELECT id FROM series_claims WHERE name = 'a'); first consensus%ROWTYPE; expected record; result record;
BEGIN
  IF laplace_attest_series('stage.attestation_test') <> 1 THEN RAISE EXCEPTION 'first claim not merged'; END IF;
  SELECT * INTO first FROM consensus WHERE claim = a;
  SELECT s.* INTO expected FROM stage.attestation_test p,
    LATERAL laplace_series(ARRAY[p.witness], ARRAY[p.games], ARRAY[p.score], ARRAY[p.trust]) s;
  IF (first.rating, first.deviation, first.volatility, first.matches) IS DISTINCT FROM
     (expected.rating, expected.deviation, expected.volatility, expected.matches)
     OR first.rating <= 1500 OR first.deviation >= 350 OR first.matches <> 1000 THEN
    RAISE EXCEPTION 'set merge differs from the Native standing';
  END IF;
  PERFORM laplace_attest_series('stage.attestation_test');
  SELECT * INTO result FROM consensus WHERE claim = a;
  IF result IS DISTINCT FROM first THEN RAISE EXCEPTION 'replay changed the standing'; END IF;
  SELECT * INTO expected FROM laplace_series(ARRAY[a], ARRAY[3000000000::bigint], ARRAY[1.0::float8], ARRAY[0.85::float8]);
  IF expected.matches <> 3000000000 OR expected.witnesses <> 1 THEN RAISE EXCEPTION 'bigint games truncated'; END IF;
END $$;
CREATE TEMP TABLE series_single AS SELECT * FROM consensus WHERE claim = (SELECT id FROM series_claims WHERE name = 'a');
-- A second claim exercises changed-window isolation during subsequent updates.
INSERT INTO stage.attestation_test SELECT c.id, w.id, NULL, 3, 1, 0.85
  FROM series_claims c, series_sources w WHERE c.name = 'b' AND w.name = 'one';
SELECT laplace_attest_series('stage.attestation_test', ARRAY[(SELECT id FROM series_claims WHERE name = 'b')]) = 1;
CREATE TEMP TABLE series_untouched AS SELECT * FROM consensus WHERE claim = (SELECT id FROM series_claims WHERE name = 'b');
SAVEPOINT native_claim_set_reader;
DO $$
DECLARE floor_claim blake3; root blake3; dog blake3:=laplace_id('dog');
        floor_id blake3:=laplace_cp_id(ascii('!')); first_id blake3;
BEGIN
  floor_claim:=pg_temp.series_node(ARRAY[floor_id,laplace_id('is'),laplace_id('noun')],'claim');
  root:=pg_temp.series_node(ARRAY[floor_claim,laplace_id('three')],'record',ARRAY[2147483648,0]::bigint[]);
  INSERT INTO witness VALUES(root,NULL,0.85);
  INSERT INTO stage.attestation_test VALUES(floor_claim,root,NULL,1,1,0.85);
  PERFORM laplace_attest_series('stage.attestation_test',ARRAY[floor_claim]);
  IF EXISTS(SELECT FROM entity WHERE id=floor_id)
     OR (SELECT count(*) FROM laplace_claims_each(ARRAY[floor_id],NULL,'{}','{}'))<>1
     OR (SELECT count(*) FROM laplace_claims(ARRAY[floor_id],NULL,'{}','{}'))<>1 THEN
    RAISE EXCEPTION 'Native claim reader lost the virtual floor or materialized it';
  END IF;
  IF (SELECT count(*) FROM laplace_claims_each(ARRAY[dog,dog],NULL,'{}','{}'))<>4
     OR (SELECT count(DISTINCT i) FROM laplace_claims_each(ARRAY[dog,dog],NULL,'{}','{}'))<>2
     OR (SELECT count(*) FROM laplace_claims_each(ARRAY[dog],1,'{}','{}'))<>1
     OR EXISTS(SELECT FROM laplace_claims_each(ARRAY[dog],0,'{}','{}'))
     OR EXISTS(SELECT FROM laplace_claims_each(NULL,NULL,'{}','{}'))
     OR EXISTS(SELECT FROM laplace_claims_each(ARRAY[dog],NULL,'{}',ARRAY[laplace_id('is')])) THEN
    RAISE EXCEPTION 'Native occurrence, caller fan, NULL or refusal contract changed';
  END IF;
  SELECT entity INTO first_id FROM laplace_claims(ARRAY[dog],NULL,'{}','{}') ORDER BY entity LIMIT 1;
  IF first_id IS DISTINCT FROM (SELECT entity FROM laplace_claims(ARRAY[dog],1,'{}','{}')) THEN
    RAISE EXCEPTION 'bounded Native claim selection changed its declared ID order';
  END IF;
END $$;
ROLLBACK TO native_claim_set_reader;
RELEASE native_claim_set_reader;
INSERT INTO stage.attestation_test SELECT c.id, w.id, NULL, 1000, 1, 0.85
  FROM series_claims c, series_sources w WHERE c.name = 'a' AND w.name = 'two';
DO $$
DECLARE a blake3 := (SELECT id FROM series_claims WHERE name = 'a'); result consensus%ROWTYPE;
BEGIN
  PERFORM laplace_attest_series('stage.attestation_test', ARRAY[a]);
  SELECT * INTO result FROM consensus WHERE claim = a;
  IF result.rating <= 1832.828 OR result.deviation >= (SELECT deviation FROM series_single) OR result.matches <> 2000 THEN
    RAISE EXCEPTION 'independent agreement did not exceed one source''s limiting rating and reduce uncertainty';
  END IF;
  IF EXISTS ((SELECT * FROM consensus WHERE claim = (SELECT claim FROM series_untouched)) EXCEPT SELECT * FROM series_untouched) THEN
    RAISE EXCEPTION 'changed window updated a different claim';
  END IF;
END $$;
CREATE TEMP TABLE series_independent AS SELECT * FROM consensus WHERE claim = (SELECT id FROM series_claims WHERE name = 'a');
-- A derived source shares root one: 1000+1000 games at that root, not another
-- independent vote. The canonical Native fold must give exactly the same bits.
INSERT INTO stage.attestation_test SELECT c.id, w.id, root.id, 1000, 1, 0.85
  FROM series_claims c, series_sources w, series_sources root WHERE c.name = 'a' AND w.name = 'three' AND root.name = 'one';
DO $$
DECLARE a blake3 := (SELECT id FROM series_claims WHERE name = 'a'); one blake3 := (SELECT id FROM series_sources WHERE name = 'one');
        two blake3 := (SELECT id FROM series_sources WHERE name = 'two'); result consensus%ROWTYPE; expected record;
BEGIN
  PERFORM laplace_attest_series('stage.attestation_test', ARRAY[a]);
  SELECT * INTO result FROM consensus WHERE claim = a;
  SELECT * INTO expected FROM laplace_series(ARRAY[one,two], ARRAY[2000,1000]::bigint[], ARRAY[1,1]::float8[], ARRAY[0.85,0.85]::float8[]);
  IF (result.rating, result.deviation, result.volatility, result.matches) IS DISTINCT FROM
     (expected.rating, expected.deviation, expected.volatility, expected.matches) OR expected.witnesses <> 2 THEN
    RAISE EXCEPTION 'dependent witnesses were not folded before the solve';
  END IF;
END $$;
CREATE TEMP TABLE series_folded AS SELECT * FROM consensus WHERE claim = (SELECT id FROM series_claims WHERE name = 'a');
-- Reverse physical staging order, then replay: every standing bit is identical.
CREATE TEMP TABLE series_reordered AS SELECT * FROM stage.attestation_test ORDER BY witness DESC;
TRUNCATE stage.attestation_test;
INSERT INTO stage.attestation_test SELECT * FROM series_reordered;
DO $$
BEGIN
  PERFORM laplace_attest_series('stage.attestation_test', ARRAY[(SELECT id FROM series_claims WHERE name = 'a')]);
  IF EXISTS ((SELECT * FROM consensus WHERE claim = (SELECT claim FROM series_folded)) EXCEPT SELECT * FROM series_folded) THEN
    RAISE EXCEPTION 'staging order changed the standing';
  END IF;
END $$;
-- Register the real source trunks. The upward GIN walk reads their record M
-- runs; pending rows overlay that same evidence and must not double its games.
INSERT INTO witness SELECT id, CASE WHEN name = 'three' THEN (SELECT id FROM series_sources WHERE name = 'one') END, 0.85 FROM series_sources;
DO $$
DECLARE a blake3 := (SELECT id FROM series_claims WHERE name = 'a'); result consensus%ROWTYPE;
BEGIN
  IF (SELECT count(*) FROM laplace_attested(ARRAY[a]) WHERE witness AND games = 1000 AND score = 1000) <> 3 THEN
    RAISE EXCEPTION 'real containment did not recover all three source series';
  END IF;
  PERFORM laplace_attest_series('stage.attestation_test', ARRAY[a]);
  SELECT * INTO result FROM consensus WHERE claim = a;
  IF result IS DISTINCT FROM (SELECT s FROM series_folded s) THEN RAISE EXCEPTION 'pending and recorded evidence counted twice'; END IF;
  DELETE FROM stage.attestation_test;
  PERFORM laplace_attest_series('stage.attestation_test', ARRAY[a]);
  SELECT * INTO result FROM consensus WHERE claim = a;
  IF result IS DISTINCT FROM (SELECT s FROM series_folded s) THEN RAISE EXCEPTION 'containment handoff changed the standing'; END IF;
END $$;
-- Two-hop lineage, then a cycle: transitive copies fold; cycles fail atomically.
UPDATE witness SET lineage = (SELECT id FROM series_sources WHERE name = 'one') WHERE id = (SELECT id FROM series_sources WHERE name = 'two');
UPDATE witness SET lineage = (SELECT id FROM series_sources WHERE name = 'two') WHERE id = (SELECT id FROM series_sources WHERE name = 'three');
DO $$
DECLARE a blake3 := (SELECT id FROM series_claims WHERE name = 'a'); result consensus%ROWTYPE; expected record; before_cycle consensus%ROWTYPE;
BEGIN
  PERFORM laplace_attest_series('stage.attestation_test', ARRAY[a]);
  SELECT * INTO result FROM consensus WHERE claim = a;
  SELECT * INTO expected FROM laplace_series(ARRAY[(SELECT id FROM series_sources WHERE name = 'one')], ARRAY[3000]::bigint[], ARRAY[1]::float8[], ARRAY[0.85]::float8[]);
  IF (result.rating, result.deviation, result.volatility, result.matches) IS DISTINCT FROM
     (expected.rating, expected.deviation, expected.volatility, expected.matches) THEN RAISE EXCEPTION 'transitive lineage did not fold'; END IF;
  before_cycle := result;
  BEGIN
    UPDATE witness SET lineage = (SELECT id FROM series_sources WHERE name = 'three') WHERE id = (SELECT id FROM series_sources WHERE name = 'one');
    PERFORM laplace_attest_series('stage.attestation_test', ARRAY[a]);
    RAISE EXCEPTION 'cyclic lineage was accepted';
  EXCEPTION WHEN null_value_not_allowed THEN NULL;
  END;
  SELECT * INTO result FROM consensus WHERE claim = a;
  IF result IS DISTINCT FROM before_cycle THEN RAISE EXCEPTION 'cyclic lineage partially updated consensus'; END IF;
  UPDATE witness SET trust = -0.85;
  PERFORM laplace_attest_series('stage.attestation_test', ARRAY[a]);
  SELECT * INTO result FROM consensus WHERE claim = a;
  IF abs(result.rating + before_cycle.rating - 3000) > 1e-8 OR abs(result.deviation - before_cycle.deviation) > 1e-8 THEN
    RAISE EXCEPTION 'signed trust did not reverse the outcome symmetrically';
  END IF;
  DELETE FROM witness;
  PERFORM laplace_attest_series('stage.attestation_test', ARRAY[a]);
  IF EXISTS (SELECT FROM consensus WHERE claim = a) THEN RAISE EXCEPTION 'standing survived removal of every witness'; END IF;
END $$;
DO $$
DECLARE id blake3 := (SELECT id FROM series_claims WHERE name = 'a');
BEGIN
  BEGIN
    PERFORM laplace_series(ARRAY[id], ARRAY[1,2]::bigint[], ARRAY[1]::float8[], ARRAY[0.85]::float8[]);
    RAISE EXCEPTION 'mismatched arrays accepted';
  EXCEPTION WHEN array_subscript_error THEN NULL; END;
  BEGIN
    PERFORM laplace_series(ARRAY[id], ARRAY[1]::bigint[], ARRAY['NaN'::float8], ARRAY[0.85]::float8[]);
    RAISE EXCEPTION 'NaN score accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    PERFORM laplace_attest_series('series_folded');
    RAISE EXCEPTION 'non-stage table accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  INSERT INTO stage.attestation_test VALUES (laplace_id('dog'), id, NULL, 1, 1, 0.85);
  BEGIN
    PERFORM laplace_attest_series('stage.attestation_test');
    RAISE EXCEPTION 'content admitted to consensus as a claim';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
-- Registered sources remain witnesses inside another trunk. Global tier
-- metadata, duplicate inputs and deep packaging must not hide containment.
DO $$
DECLARE a blake3 := (SELECT id FROM series_claims WHERE name = 'a');
        inner_source blake3 := (SELECT id FROM series_sources WHERE name = 'one');
        outer_source blake3; deep_source blake3; found integer;
BEGIN
  INSERT INTO witness VALUES (inner_source,NULL,0.85);
  outer_source := pg_temp.series_node(ARRAY[laplace_id('three'),inner_source], 'file', ARRAY[4294967296,0]::bigint[]);
  INSERT INTO witness VALUES (outer_source,NULL,0.85);
  UPDATE entity SET tier=1 WHERE id=outer_source;
  UPDATE physicality SET tier=1 WHERE entity=outer_source;
  SELECT count(*) INTO found FROM laplace_attested(ARRAY[a,a])
    WHERE witness AND trunk IN (inner_source,outer_source) AND games=1000 AND score=1000;
  IF found<>2 THEN RAISE EXCEPTION 'nested witness, duplicate input or global tier lost provenance'; END IF;
  deep_source := outer_source;
  FOR i IN 1..70 LOOP
    deep_source := pg_temp.series_node(ARRAY[laplace_id('one'),deep_source], 'file', ARRAY[4294967296,0]::bigint[]);
  END LOOP;
  INSERT INTO witness VALUES (deep_source,NULL,0.85);
  SELECT count(*) INTO found FROM laplace_attested(ARRAY[a])
    WHERE witness AND trunk IN (inner_source,outer_source,deep_source) AND games=1000 AND score=1000;
  IF found<>3 THEN RAISE EXCEPTION 'deep containment truncated a registered witness'; END IF;
  SELECT count(*) INTO found FROM laplace_contained_series(ARRAY[inner_source,outer_source,deep_source])
    WHERE claim=a AND games=1000 AND score=1000;
  IF found<>3 THEN RAISE EXCEPTION 'downward traversal lost nested roots or relied on global tier'; END IF;
  SELECT count(*) INTO found FROM laplace_contained_series(ARRAY[inner_source,outer_source,deep_source],ARRAY[a])
    WHERE games=1000 AND score=1000;
  IF found<>3 THEN RAISE EXCEPTION 'selected series lost nested registered witnesses'; END IF;
  IF EXISTS(SELECT FROM laplace_contained_series(ARRAY[deep_source],ARRAY[laplace_id('dog')])) THEN
    RAISE EXCEPTION 'reverse selection admitted ordinary content as a claim';
  END IF;
  SELECT count(*) INTO found FROM laplace_attested(ARRAY[a],true) evidence
    JOIN physicality p ON p.entity=evidence.trunk
    WHERE NOT evidence.witness AND evidence.games=1000
      AND laplace_path_starts(p.path,laplace_compose(ARRAY[laplace_id('file'),laplace_id('one')]));
  IF found<>1 THEN RAISE EXCEPTION 'file diagnostic omitted the containing file'; END IF;
END $$;
-- One corpus may contain curated and calculated testimony. Calculation is a
-- contained registry-class qualifier, not a second witness. Copies of that
-- calculation in another corpus still share its dependence root.
DO $$
DECLARE c blake3; cls blake3 := laplace_text_id('DerivedCalculation');
        calculated blake3; curated blake3; first_source blake3; second_source blake3;
        expected record; actual consensus%ROWTYPE; found integer;
BEGIN
  INSERT INTO entity (id,tier,coord,hilbert) VALUES
    (cls,laplace_tier('DerivedCalculation'),laplace_coord('DerivedCalculation'),laplace_hilbert('DerivedCalculation'));
  INSERT INTO physicality (entity,tier,hilbert,path) VALUES
    (cls,laplace_tier('DerivedCalculation'),laplace_hilbert('DerivedCalculation'),
     laplace_path(ARRAY(SELECT laplace_cp_id(ascii(ch)) FROM regexp_split_to_table('DerivedCalculation','') ch)));
  c := pg_temp.series_node(ARRAY[laplace_id('source'),laplace_id('is'),laplace_id('noun')],'claim');
  calculated := pg_temp.series_node(ARRAY[cls,c,laplace_id('one')],'record',ARRAY[7516192768,1073741824,0]::bigint[]);
  curated := pg_temp.series_node(ARRAY[c,laplace_id('two')],'record',ARRAY[1073741824,0]::bigint[]);
  first_source := pg_temp.series_node(ARRAY[laplace_id('source'),calculated,curated],'file',ARRAY[4294967296,5368709120,5368709120]::bigint[]);
  second_source := pg_temp.series_node(ARRAY[laplace_id('three'),calculated],'file',ARRAY[4294967296,5368709120]::bigint[]);
  INSERT INTO witness VALUES (first_source,NULL,0.85),(second_source,NULL,0.85);
  SELECT count(*) INTO found FROM laplace_attested_series(ARRAY[c])
    WHERE witness AND dependence_root=cls AND trust=0.70 AND games=1 AND score=1;
  IF found<>2 THEN RAISE EXCEPTION 'contained calculation did not retain its class root and trust'; END IF;
  SELECT count(*) INTO found FROM laplace_contained_series(ARRAY[first_source,second_source])
    WHERE claim=c AND dependence_root=cls AND dependence_trust=0.70 AND games=1 AND score=1;
  IF found<>2 THEN RAISE EXCEPTION 'downward calculation lost its dependence root or trust'; END IF;
  SELECT count(*) INTO found FROM laplace_contained_series(ARRAY[first_source,second_source],ARRAY[c])
    WHERE dependence_root=cls AND dependence_trust=0.70 AND games=1 AND score=1;
  IF found<>2 THEN RAISE EXCEPTION 'selected calculation lost its dependence root or trust'; END IF;
  SELECT count(*) INTO found FROM laplace_attested(ARRAY[c])
    WHERE witness AND trunk=first_source AND games=2 AND score=2 AND trust=0.85;
  IF found<>1 THEN RAISE EXCEPTION 'source provenance created extra calculation witnesses'; END IF;
  SELECT s.* INTO expected FROM laplace_series(ARRAY[first_source,cls,cls],ARRAY[1,1,1]::bigint[],
    ARRAY[1,1,1]::float8[],ARRAY[0.85,0.70,0.70]::float8[]) s;
  CREATE UNLOGGED TABLE stage.attestation_scope_test (LIKE stage.attestation_test INCLUDING DEFAULTS);
  PERFORM laplace_attest_series('stage.attestation_scope_test',ARRAY[c]);
  SELECT * INTO actual FROM consensus WHERE claim=c;
  IF ROW(float8send(actual.rating),float8send(actual.deviation),float8send(actual.volatility),actual.matches)
     IS DISTINCT FROM ROW(float8send(expected.rating),float8send(expected.deviation),float8send(expected.volatility),expected.matches)
  THEN RAISE EXCEPTION 'stored dependence scopes disagree with Native rooted solving'; END IF;
  INSERT INTO stage.attestation_scope_test VALUES (c,first_source,NULL,1,1,0.85),(c,first_source,cls,1,1,0.70);
  PERFORM laplace_attest_series('stage.attestation_scope_test',ARRAY[c]);
  PERFORM laplace_attest_series('stage.attestation_scope_test',ARRAY[c]);
  SELECT * INTO actual FROM consensus WHERE claim=c;
  IF ROW(float8send(actual.rating),float8send(actual.deviation),float8send(actual.volatility),actual.matches)
     IS DISTINCT FROM ROW(float8send(expected.rating),float8send(expected.deviation),float8send(expected.volatility),expected.matches)
  THEN RAISE EXCEPTION 'cumulative multi-root source replay duplicated games or replaced calculation trust'; END IF;
  BEGIN
    PERFORM pg_temp.series_node(ARRAY[laplace_id('dog'),c],'record',ARRAY[7516192768,1073741824]::bigint[]);
    PERFORM * FROM laplace_attested_series(ARRAY[c]);
    RAISE EXCEPTION 'unknown dependence class accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
SELECT 'dependence: source witness retained; stored and cumulative class-rooted series agree exactly';
-- Claim semantics come from the carrier kind, never a triple-only shape.
-- Exercise the same cumulative and persisted containment path at arities two
-- and four, including a four-part claim stored as one RLE vertex.
DO $$
DECLARE binary_claim blake3; joint_claim blake3; repeated_claim blake3;
        record_node blake3; source_node blake3; unasserted blake3; c blake3;
        wanted record; actual consensus%ROWTYPE; first consensus%ROWTYPE; found integer;
BEGIN
  binary_claim := pg_temp.series_node(ARRAY[laplace_id('dog'),laplace_id('noun')],'claim');
  joint_claim := pg_temp.series_node(ARRAY[laplace_id('dog'),laplace_id('is'),laplace_id('noun'),laplace_id('two')],'claim');
  repeated_claim := pg_temp.series_node(array_fill(laplace_id('dog'),ARRAY[4]),'claim');
  IF NOT EXISTS (SELECT FROM physicality WHERE entity=repeated_claim
                 AND laplace_path_length(path)=4 AND ST_NPoints(path)=1) THEN
    RAISE EXCEPTION 'n-ary RLE fixture lost logical arity';
  END IF;
  record_node := pg_temp.series_node(ARRAY[binary_claim,joint_claim,repeated_claim],'record',
                                     ARRAY[1073741824,1073741824,1073741824]::bigint[]);
  source_node := pg_temp.series_node(ARRAY[laplace_id('source'),record_node],'file',
                                     ARRAY[4294967296,5368709120]::bigint[]);
  INSERT INTO witness VALUES (source_node,NULL,0.85);
  CREATE UNLOGGED TABLE stage.attestation_arity_test (LIKE stage.attestation_test INCLUDING DEFAULTS);
  INSERT INTO stage.attestation_arity_test
    SELECT id,source_node,NULL,1,1,0.85 FROM unnest(ARRAY[binary_claim,joint_claim,repeated_claim]) id;
  IF laplace_attest_series('stage.attestation_arity_test') <> 3 THEN
    RAISE EXCEPTION 'binary or n-ary claim was not admitted';
  END IF;
  SELECT count(*) INTO found FROM laplace_contained_series(ARRAY[source_node])
    WHERE claim=ANY(ARRAY[binary_claim,joint_claim,repeated_claim]) AND games=1 AND score=1;
  IF found<>3 THEN RAISE EXCEPTION 'downward Native traversal lost binary or n-ary RLE claims'; END IF;
  FOREACH c IN ARRAY ARRAY[binary_claim,joint_claim,repeated_claim] LOOP
    SELECT * INTO first FROM consensus WHERE claim=c;
    SELECT * INTO wanted FROM laplace_series(ARRAY[source_node],ARRAY[1]::bigint[],ARRAY[1]::float8[],ARRAY[0.85]::float8[]);
    IF first.claim IS NULL OR ROW(float8send(first.rating),float8send(first.deviation),float8send(first.volatility),first.matches)
       IS DISTINCT FROM ROW(float8send(wanted.rating),float8send(wanted.deviation),float8send(wanted.volatility),wanted.matches) THEN
      RAISE EXCEPTION 'n-ary cumulative standing differs from Native';
    END IF;
    SELECT count(*) INTO found FROM laplace_attested_series(ARRAY[c])
      WHERE witness AND trunk=source_node AND games=1 AND score=1 AND trust=0.85;
    IF found<>1 THEN RAISE EXCEPTION 'n-ary claim lost its source containment'; END IF;
    PERFORM laplace_attest_series('stage.attestation_arity_test',ARRAY[c]);
    SELECT * INTO actual FROM consensus WHERE claim=c;
    IF actual IS DISTINCT FROM first THEN RAISE EXCEPTION 'n-ary cumulative replay added a game'; END IF;
    DELETE FROM stage.attestation_arity_test WHERE claim=c;
    PERFORM laplace_attest_series('stage.attestation_arity_test',ARRAY[c]);
    SELECT * INTO actual FROM consensus WHERE claim=c;
    IF actual IS DISTINCT FROM first THEN RAISE EXCEPTION 'n-ary stored containment disagrees with cumulative input'; END IF;
  END LOOP;
  unasserted := pg_temp.series_node(ARRAY[laplace_id('noun'),laplace_id('two'),laplace_id('is'),laplace_id('dog')],'tuple');
  INSERT INTO stage.attestation_arity_test VALUES (unasserted,source_node,NULL,1,1,0.85);
  BEGIN
    PERFORM laplace_attest_series('stage.attestation_arity_test');
    RAISE EXCEPTION 'a non-claim n-ary tuple acquired a standing';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
-- More than one cursor fetch must preserve every registered contribution.
-- These canonical RLE containers have no witness, so they cannot alter shares.
CREATE TEMP TABLE cursor_expected AS
SELECT * FROM laplace_attribution(ARRAY[(SELECT id FROM series_claims WHERE name='a')]);
CREATE TEMP TABLE cursor_records(id blake3);
CREATE TEMP TABLE cursor_root(id blake3);
DO $$
DECLARE a blake3 := (SELECT id FROM series_claims WHERE name='a');
        tail blake3 := laplace_id('one'); i integer;
BEGIN
  FOR i IN 1..2049 LOOP
    INSERT INTO cursor_records SELECT pg_temp.series_node(ARRAY[a] || array_fill(tail,ARRAY[i]),'record',ARRAY[1073741824,0]::bigint[]);
  END LOOP;
  INSERT INTO cursor_root SELECT pg_temp.series_node(ARRAY(SELECT id FROM cursor_records ORDER BY id),'tuple');
END $$;
SET LOCAL work_mem='64kB';
CREATE TEMP TABLE cursor_actual AS
SELECT * FROM laplace_attribution(ARRAY[(SELECT id FROM series_claims WHERE name='a')]);
DO $$ BEGIN
  IF EXISTS ((TABLE cursor_actual EXCEPT ALL TABLE cursor_expected)
             UNION ALL (TABLE cursor_expected EXCEPT ALL TABLE cursor_actual)) THEN
    RAISE EXCEPTION 'cursor chunks changed registered witness attribution';
  END IF;
END $$;
-- The downward frontier exceeds a 64kB parameter window and result fetch.
-- It must still reconstruct every real assertion, including the final window.
DO $$
DECLARE root blake3 := (SELECT id FROM cursor_root);
        a blake3 := (SELECT id FROM series_claims WHERE name='a');
        found bigint;
BEGIN
  SELECT count(*) INTO found FROM laplace_contained_series(ARRAY[root])
    WHERE claim=a AND games=2049 AND score=2049;
  IF found<>1 THEN RAISE EXCEPTION 'downward cursor lost assertions across memory windows'; END IF;
  IF EXISTS (
    (SELECT claim,trunk,games,tokens,score,"position",dependence_root FROM laplace_contained_series(ARRAY[root])
     EXCEPT ALL
     SELECT claim,trunk,games,tokens,score,"position",dependence_root FROM laplace_attested_series(ARRAY[a]) WHERE trunk=root)
    UNION ALL
    (SELECT claim,trunk,games,tokens,score,"position",dependence_root FROM laplace_attested_series(ARRAY[a]) WHERE trunk=root
     EXCEPT ALL
     SELECT claim,trunk,games,tokens,score,"position",dependence_root FROM laplace_contained_series(ARRAY[root]))
  ) THEN RAISE EXCEPTION 'downward memory windows changed Native testimony'; END IF;
END $$;

-- A large frontier remains a parameter of bounded indexed Native windows.
-- Even under forced outer parallel planning, the Native walk owns its
-- frontier execution and must not start a worker fleet at every hop/window.
CREATE TEMP TABLE frontier_expected AS
SELECT * FROM laplace_attribution(ARRAY(SELECT id FROM series_claims ORDER BY name));
CREATE TEMP TABLE frontier_actual (LIKE frontier_expected);
DO $$ BEGIN
  IF (SELECT count(DISTINCT claim) FROM frontier_expected) <> 2
     OR EXISTS (SELECT FROM frontier_expected GROUP BY claim HAVING sum(share_bp) IS DISTINCT FROM 10000) THEN
    RAISE EXCEPTION 'large-frontier fixture has no complete witness attribution';
  END IF;
END $$;
SET LOCAL debug_parallel_query = on;
SET LOCAL max_parallel_workers_per_gather = 1;
SET LOCAL statement_timeout = '60s';
-- The real fold is an INSERT. Keep the outer command nonparallel so the
-- containment query itself, rather than the entire function, runs in workers.
INSERT INTO frontier_actual
SELECT * FROM laplace_attribution(
  ARRAY[(SELECT id FROM series_claims WHERE name='a')]
  || ARRAY(SELECT md5(i::text)::blake3 FROM generate_series(1,120000) i)
  || ARRAY[(SELECT id FROM series_claims WHERE name='b')]);
DO $$ BEGIN
  IF EXISTS (
    SELECT FROM (
      (TABLE frontier_actual EXCEPT ALL TABLE frontier_expected)
      UNION ALL
      (TABLE frontier_expected EXCEPT ALL TABLE frontier_actual)
    ) differences
  ) THEN RAISE EXCEPTION 'large frontier changed witness attribution'; END IF;
END $$;
-- Direct Native primary-index reads retain parent-column privileges. A
-- reader of IDs/masks cannot acquire paths through the containment function.
DO $$
DECLARE reader text := 'laplace_frontier_reader_' || pg_backend_pid();
        owner_name text := current_user;
        root blake3 := (SELECT id FROM cursor_root);
        a blake3 := (SELECT id FROM series_claims WHERE name='a');
        denied boolean := false;found bigint;
BEGIN
  EXECUTE format('CREATE ROLE %I',reader);
  EXECUTE format('GRANT USAGE ON SCHEMA public TO %I',reader);
  EXECUTE format('GRANT SELECT(entity,mask) ON public.physicality TO %I',reader);
  EXECUTE format('SET LOCAL ROLE %I',reader);
  BEGIN
    PERFORM count(*) FROM laplace_contained_series(ARRAY[root]);
  EXCEPTION WHEN insufficient_privilege THEN denied := true;
  END;
  EXECUTE format('SET LOCAL ROLE %I',owner_name);
  IF NOT denied THEN RAISE EXCEPTION 'Native frontier bypassed path-column privileges'; END IF;
  EXECUTE format('GRANT SELECT(path) ON public.physicality TO %I',reader);
  EXECUTE format('SET LOCAL ROLE %I',reader);
  SELECT count(*) INTO found FROM laplace_contained_series(ARRAY[root])
    WHERE claim=a AND games=2049 AND score=2049;
  EXECUTE format('SET LOCAL ROLE %I',owner_name);
  IF found<>1 THEN RAISE EXCEPTION 'column-scoped Native frontier lost testimony'; END IF;
  ALTER TABLE public.physicality ENABLE ROW LEVEL SECURITY;
  denied := false;
  EXECUTE format('SET LOCAL ROLE %I',reader);
  BEGIN
    PERFORM count(*) FROM laplace_contained_series(ARRAY[root]);
  EXCEPTION WHEN feature_not_supported THEN denied := true;
  END;
  EXECUTE format('SET LOCAL ROLE %I',owner_name);
  ALTER TABLE public.physicality DISABLE ROW LEVEL SECURITY;
  IF NOT denied THEN RAISE EXCEPTION 'Native frontier bypassed active row-level security'; END IF;
END $$;

SELECT 'attestation: large frontier preserves exact witness attribution';
ROLLBACK;
SELECT 'attestation: real containment, cumulative replay, independent agreement, rooted folding, lineage, signed trust and bigint games passed';
