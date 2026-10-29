-- Request positions, exact content and access policy are shared by every
-- client of the native set reader; partition names are not part of its API.
BEGIN;
INSERT INTO entity(id,tier,coord,hilbert)
SELECT laplace_id('stored row reader'),laplace_tier('stored row reader'),
       laplace_coord('stored row reader'),laplace_hilbert('stored row reader');
INSERT INTO physicality(entity,tier,hilbert,path)
SELECT id,tier,hilbert,laplace_path(ARRAY[laplace_cp_id(97),laplace_cp_id(98),laplace_cp_id(98)])
FROM entity WHERE id=laplace_id('stored row reader');
DO $$
DECLARE known blake3:=laplace_id('stored row reader');
        missing blake3:='00000000000000000000000000000000';
        requested blake3[];
BEGIN
 requested:=ARRAY[known,missing,known];
 IF (SELECT array_agg(ordinal ORDER BY ordinal) FROM laplace_entities(requested))
    IS DISTINCT FROM ARRAY[1,3]::bigint[]
 OR (SELECT array_agg(ordinal ORDER BY ordinal) FROM laplace_physicalities(requested))
    IS DISTINCT FROM ARRAY[1,3]::bigint[] THEN
  RAISE EXCEPTION 'stored-row request positions or absent IDs changed';
 END IF;
 IF EXISTS(SELECT FROM laplace_entities(requested) r JOIN entity e ON e.id=r.id
           WHERE r.tier<>e.tier OR r.hilbert<>e.hilbert OR r.mask<>e.mask
              OR ST_AsEWKB(r.coord)<>ST_AsEWKB(e.coord))
 OR EXISTS(SELECT FROM laplace_physicalities(requested) r JOIN physicality p ON p.entity=r.entity
           WHERE r.tier<>p.tier OR r.hilbert<>p.hilbert OR r.mask<>p.mask
              OR ST_AsEWKB(r.path)<>ST_AsEWKB(p.path)) THEN
  RAISE EXCEPTION 'stored-row carrier differs from PostgreSQL';
 END IF;
 IF EXISTS(SELECT FROM laplace_entities(ARRAY[]::blake3[]))
 OR EXISTS(SELECT FROM laplace_physicalities(ARRAY[]::blake3[]))
 OR EXISTS(SELECT FROM laplace_entities(NULL)) THEN
  RAISE EXCEPTION 'empty stored-row request has rows';
 END IF;
END $$;
DO $$ DECLARE reader text:=current_database()||'_stored_reader'; denied boolean:=false;
BEGIN
 EXECUTE format('CREATE ROLE %I',reader);
 EXECUTE format('GRANT USAGE ON SCHEMA public TO %I',reader);
 EXECUTE format('GRANT SELECT(id,tier,coord,hilbert,mask) ON entity TO %I',reader);
 EXECUTE format('GRANT SELECT(entity,tier,hilbert,path,mask) ON physicality TO %I',reader);
 EXECUTE format('SET LOCAL ROLE %I',reader);
 IF (SELECT count(*) FROM laplace_entities(ARRAY[laplace_id('stored row reader')]))<>1
 OR (SELECT count(*) FROM laplace_physicalities(ARRAY[laplace_id('stored row reader')]))<>1 THEN
  RAISE EXCEPTION 'column-authorized reader lost stored rows';
 END IF;
 RESET ROLE;
 EXECUTE format('REVOKE SELECT(coord) ON entity FROM %I',reader);
 EXECUTE format('SET LOCAL ROLE %I',reader);
 BEGIN
  PERFORM laplace_entities(ARRAY[laplace_id('stored row reader')]);
 EXCEPTION WHEN insufficient_privilege THEN denied:=true;
 END;
 RESET ROLE;
 IF NOT denied THEN RAISE EXCEPTION 'stored-row reader bypassed column ACL'; END IF;
 ALTER TABLE physicality ENABLE ROW LEVEL SECURITY;
 EXECUTE format('SET LOCAL ROLE %I',reader);denied:=false;
 BEGIN
  PERFORM laplace_physicalities(ARRAY[laplace_id('stored row reader')]);
 EXCEPTION WHEN feature_not_supported THEN denied:=true;
 END;
 RESET ROLE;
 IF NOT denied THEN RAISE EXCEPTION 'stored-row reader bypassed RLS boundary'; END IF;
END $$;
-- Renaming a partition must not make canonical content disappear.
DO $$ DECLARE child regclass; BEGIN
 SELECT tableoid::regclass INTO STRICT child FROM entity WHERE id=laplace_id('stored row reader');
 EXECUTE format('ALTER TABLE %s RENAME TO renamed_entity_range',child);
 SELECT tableoid::regclass INTO STRICT child FROM physicality WHERE entity=laplace_id('stored row reader');
 EXECUTE format('ALTER TABLE %s RENAME TO renamed_path_range',child);
 IF (SELECT count(*) FROM laplace_entities(ARRAY[laplace_id('stored row reader')]))<>1
 OR (SELECT count(*) FROM laplace_physicalities(ARRAY[laplace_id('stored row reader')]))<>1 THEN
  RAISE EXCEPTION 'set reader depends on a partition name';
 END IF;
END $$;
ROLLBACK;
