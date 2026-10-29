-- The same complete containment sets serve CLI, MCP and managed consumers.
BEGIN;
CREATE TEMP TABLE holder_fixture(label text,id blake3,claim boolean);
INSERT INTO holder_fixture SELECT value,laplace_id(value),claim
FROM (VALUES('ab',true),('ac',true),('ad',false),('bb',false),('bc',true)) v(value,claim);
INSERT INTO entity(id,tier,coord,hilbert)
SELECT id,laplace_tier(label),laplace_coord(label),laplace_hilbert(label) FROM holder_fixture;
INSERT INTO physicality(entity,tier,hilbert,path,mask)
SELECT f.id,e.tier,e.hilbert,laplace_path(ARRAY[
 laplace_cp_id(ascii(substr(label,1,1))),laplace_cp_id(ascii(substr(label,2,1)))]),
 CASE WHEN claim THEN laplace_mask_slot('kind','claim') ELSE B'0'::bit(64) END
FROM holder_fixture f JOIN entity e ON e.id=f.id;
INSERT INTO consensus SELECT id,1550,60,0.06,7 FROM holder_fixture WHERE claim;
DO $$
DECLARE a blake3:=laplace_cp_id(97); b blake3:=laplace_cp_id(98); c blake3:=laplace_cp_id(99);
        keys blake3[]; before_scans bigint; after_scans bigint;
BEGIN
 keys:=ARRAY[a,b,a,'00000000000000000000000000000000'::blake3];
 SELECT coalesce(sum(seq_scan),0) INTO before_scans FROM pg_stat_xact_all_tables
 WHERE relid IN(SELECT inhrelid FROM pg_inherits WHERE inhparent='physicality'::regclass);
 IF (SELECT count(*) FROM laplace_holders(keys))<>5
 OR EXISTS(SELECT FROM laplace_holders(keys) WHERE ordinal<>0 OR hub)
 OR (SELECT count(*) FROM laplace_holders(keys,ARRAY[c]))<>2
 OR (SELECT count(*) FROM laplace_holders(keys,'{}',true))<>9
 OR (SELECT count(*) FROM laplace_holders(keys,'{}',false,1::smallint))<>0 THEN
  RAISE EXCEPTION 'Native union/per-part/required/floor containment changed';
 END IF;
 -- Equality at the boundary is complete. Overflow emits a hub marker for
 -- that kind and key only, with no arbitrary subset of its carriers.
 IF (SELECT count(*) FROM laplace_holders(keys,'{}',true,-1::smallint,2))<>9
 OR EXISTS(SELECT FROM laplace_holders(keys,'{}',true,-1::smallint,2) WHERE hub)
 OR (SELECT count(*) FROM laplace_holders(keys,'{}',true,-1::smallint,1) WHERE hub AND kind=1)<>3
 OR (SELECT count(*) FROM laplace_holders(keys,'{}',true,-1::smallint,1) WHERE NOT hub AND kind=0)<>3
 OR EXISTS(SELECT FROM laplace_holders(keys,'{}',true,-1::smallint,1) WHERE NOT hub AND kind=1)
 OR EXISTS(SELECT FROM laplace_holders(keys,'{}',true,-1::smallint,1) WHERE hub AND (entity IS NOT NULL OR path IS NOT NULL OR tier IS NOT NULL OR mask IS NOT NULL))
 OR (SELECT count(*) FROM laplace_holders(keys,'{}',true,-1::smallint,0))<>6
 OR (SELECT count(*) FROM laplace_holders(keys,'{}',true,-1::smallint,0,1::smallint))<>3 THEN
  RAISE EXCEPTION 'Native cap lost exact key/kind boundary or duplicate request positions';
 END IF;
 IF EXISTS(SELECT FROM laplace_holders('{}'::blake3[]))
 OR EXISTS(SELECT FROM laplace_holders(keys,ARRAY[c,a,b]))
 OR (SELECT count(*) FROM laplace_holders(ARRAY[b,b],ARRAY[c],true))<>2 THEN
  RAISE EXCEPTION 'Native empty or required-key selection changed';
 END IF;
 IF (SELECT array_agg(ordinal ORDER BY ordinal) FROM laplace_standings(
    ARRAY[laplace_id('ab'),'00000000000000000000000000000000'::blake3,laplace_id('ab')])) IS DISTINCT FROM ARRAY[1,3]::bigint[]
 OR EXISTS(SELECT FROM laplace_standings(ARRAY[laplace_id('ab')]) WHERE rating<>1550 OR deviation<>60 OR volatility<>0.06 OR matches<>7) THEN
  RAISE EXCEPTION 'Native standing read lost typed values, absent IDs or duplicate positions';
 END IF;
 SELECT coalesce(sum(seq_scan),0) INTO after_scans FROM pg_stat_xact_all_tables
 WHERE relid IN(SELECT inhrelid FROM pg_inherits WHERE inhparent='physicality'::regclass);
 IF after_scans<>before_scans THEN RAISE EXCEPTION 'Native holders used a sequential table scan'; END IF;
END $$;
DO $$ DECLARE reader text:=current_database()||'_holder_reader'; denied boolean:=false;
BEGIN
 EXECUTE format('CREATE ROLE %I',reader);
 EXECUTE format('GRANT USAGE ON SCHEMA public TO %I',reader);
 EXECUTE format('GRANT SELECT(entity,path,tier,mask) ON physicality TO %I',reader);
 EXECUTE format('GRANT SELECT(claim,rating,deviation,volatility,matches) ON consensus TO %I',reader);
 EXECUTE format('SET LOCAL ROLE %I',reader);
 IF (SELECT count(*) FROM laplace_holders(ARRAY[laplace_cp_id(97)]))<>3
 OR (SELECT count(*) FROM laplace_standings(ARRAY[laplace_id('ab')]))<>1 THEN
  RAISE EXCEPTION 'column-authorized Native holder or standing reader lost rows';
 END IF;
 RESET ROLE;
 EXECUTE format('REVOKE SELECT(path) ON physicality FROM %I',reader);
 EXECUTE format('SET LOCAL ROLE %I',reader);
 BEGIN PERFORM laplace_holders(ARRAY[laplace_cp_id(97)]);
 EXCEPTION WHEN insufficient_privilege THEN denied:=true; END;
 RESET ROLE;
 IF NOT denied THEN RAISE EXCEPTION 'Native holder reader bypassed path ACL'; END IF;
 EXECUTE format('GRANT SELECT(path) ON physicality TO %I',reader);
 ALTER TABLE physicality ENABLE ROW LEVEL SECURITY;
 EXECUTE format('SET LOCAL ROLE %I',reader);denied:=false;
 BEGIN PERFORM laplace_holders(ARRAY[laplace_cp_id(97)]);
 EXCEPTION WHEN feature_not_supported THEN denied:=true; END;
 RESET ROLE;
 IF NOT denied THEN RAISE EXCEPTION 'Native holder reader bypassed RLS'; END IF;
 EXECUTE format('REVOKE SELECT(rating) ON consensus FROM %I',reader);
 EXECUTE format('SET LOCAL ROLE %I',reader);denied:=false;
 BEGIN PERFORM laplace_standings(ARRAY[laplace_id('ab')]);
 EXCEPTION WHEN insufficient_privilege THEN denied:=true; END;
 RESET ROLE;
 IF NOT denied THEN RAISE EXCEPTION 'Native standing reader bypassed rating ACL'; END IF;
END $$;
ROLLBACK;
