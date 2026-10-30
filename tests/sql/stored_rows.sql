\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
-- Request positions, exact content and access policy are shared by every
-- client of the native set reader; partition names are not part of its API.
BEGIN;
INSERT INTO entity(id,tier,coord,hilbert)
SELECT laplace_id('abb'),laplace_tier('abb'),
       laplace_coord('abb'),laplace_hilbert('abb');
INSERT INTO physicality(entity,tier,hilbert,path)
SELECT id,tier,hilbert,laplace_path(ARRAY[laplace_cp_id(97),laplace_cp_id(98),laplace_cp_id(98)])
FROM entity WHERE id=laplace_id('abb');
DO $$
DECLARE known blake3:=laplace_id('abb');
        missing blake3:='00000000000000000000000000000000';
        requested blake3[];
BEGIN
 requested:=ARRAY[known,missing,known];
 IF known IS DISTINCT FROM laplace_compose(ARRAY[laplace_cp_id(97),laplace_cp_id(98),laplace_cp_id(98)]) THEN
  RAISE EXCEPTION 'stored-row fixture is not canonical';
 END IF;
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
 -- The leaves exist only in the cache. Their missing entity rows must not
 -- turn the parent's tier predicate into NULL and hide the whole frontier.
 IF (SELECT count(*) FROM laplace_containers(ARRAY[laplace_cp_id(97),laplace_cp_id(98)],'{}'::smallint[]))<>1
 OR (SELECT count(*) FROM laplace_containers(ARRAY[laplace_cp_id(97),laplace_cp_id(97),laplace_cp_id(98)],'{}'::smallint[]))<>1
 OR EXISTS(SELECT FROM laplace_containers(ARRAY[laplace_cp_id(97),laplace_cp_id(99)],'{}'::smallint[]))
 OR EXISTS(SELECT FROM laplace_containers(ARRAY[]::blake3[],'{}'::smallint[]))
 OR EXISTS(SELECT FROM laplace_containers(ARRAY[laplace_cp_id(97)],ARRAY[laplace_bank_bit('kind','claim')])) THEN
  RAISE EXCEPTION 'Native all-key containment lost the virtual floor, duplicates or mask restriction';
 END IF;
 UPDATE physicality SET mask=laplace_mask_slot('kind','claim') WHERE entity=known;
 IF (SELECT count(*) FROM laplace_containers(ARRAY[laplace_cp_id(97)],ARRAY[laplace_bank_bit('kind','claim')]))<>1 THEN
  RAISE EXCEPTION 'Native container carrier mask was not applied';
 END IF;
END $$;
DO $$ DECLARE reader text:=current_database()||'_stored_reader'; denied boolean:=false;
BEGIN
 EXECUTE format('CREATE ROLE %I',reader);
 EXECUTE format('GRANT USAGE ON SCHEMA public TO %I',reader);
 EXECUTE format('GRANT SELECT(id,tier,coord,hilbert,mask) ON entity TO %I',reader);
 EXECUTE format('GRANT SELECT(entity,tier,hilbert,path,mask) ON physicality TO %I',reader);
 EXECUTE format('SET LOCAL ROLE %I',reader);
 IF (SELECT count(*) FROM laplace_entities(ARRAY[laplace_id('abb')]))<>1
 OR (SELECT count(*) FROM laplace_physicalities(ARRAY[laplace_id('abb')]))<>1
 OR (SELECT count(*) FROM laplace_containers(ARRAY[laplace_cp_id(97)],'{}'::smallint[]))<>1 THEN
  RAISE EXCEPTION 'column-authorized reader lost stored rows';
 END IF;
 RESET ROLE;
 EXECUTE format('REVOKE SELECT(coord) ON entity FROM %I',reader);
 EXECUTE format('SET LOCAL ROLE %I',reader);
 BEGIN
  PERFORM laplace_entities(ARRAY[laplace_id('abb')]);
 EXCEPTION WHEN insufficient_privilege THEN denied:=true;
 END;
 RESET ROLE;
 IF NOT denied THEN RAISE EXCEPTION 'stored-row reader bypassed column ACL'; END IF;
 ALTER TABLE physicality ENABLE ROW LEVEL SECURITY;
 EXECUTE format('SET LOCAL ROLE %I',reader);denied:=false;
 BEGIN
  PERFORM laplace_physicalities(ARRAY[laplace_id('abb')]);
 EXCEPTION WHEN feature_not_supported THEN denied:=true;
 END;
 RESET ROLE;
 IF NOT denied THEN RAISE EXCEPTION 'stored-row reader bypassed RLS boundary'; END IF;
END $$;
-- Renaming a partition must not make canonical content disappear.
DO $$ DECLARE child regclass; bounds text; BEGIN
 SELECT tableoid::regclass INTO STRICT child FROM entity WHERE id=laplace_id('abb');
 EXECUTE format('ALTER TABLE %s RENAME TO renamed_entity_range',child);
 SELECT tableoid::regclass INTO STRICT child FROM physicality WHERE entity=laplace_id('abb');
 EXECUTE format('ALTER TABLE %s RENAME TO renamed_path_range',child);
 IF (SELECT count(*) FROM laplace_entities(ARRAY[laplace_id('abb')]))<>1
 OR (SELECT count(*) FROM laplace_physicalities(ARRAY[laplace_id('abb')]))<>1 THEN
  RAISE EXCEPTION 'set reader depends on a partition name';
 END IF;
 -- PostgreSQL permits attached partitions to store the inherited columns in
 -- another physical order. The Native reader projects the parent's contract.
 -- Rebalance both sides under this transaction, then validate the FK again.
 ALTER TABLE physicality DROP CONSTRAINT physicality_entity_fkey;
 SELECT pg_get_expr(relpartbound,oid) INTO STRICT bounds FROM pg_class WHERE oid='renamed_entity_range'::regclass;
 EXECUTE 'ALTER TABLE entity DETACH PARTITION renamed_entity_range';
 CREATE TABLE reordered_entity_range AS SELECT mask,hilbert,coord,tier,id FROM renamed_entity_range;
 ALTER TABLE reordered_entity_range ALTER COLUMN id SET NOT NULL,
   ALTER COLUMN tier SET NOT NULL,ALTER COLUMN coord SET NOT NULL,
   ALTER COLUMN hilbert SET NOT NULL,ALTER COLUMN mask SET NOT NULL,ADD PRIMARY KEY(id),
   ADD CONSTRAINT entity_mask_valid CHECK(laplace_mask_valid(mask));
 EXECUTE 'ALTER TABLE entity ATTACH PARTITION reordered_entity_range '||bounds;
 IF (SELECT count(*) FROM laplace_entities(ARRAY[laplace_id('abb')]))<>1
 OR (SELECT tier FROM laplace_entities(ARRAY[laplace_id('abb')]))<>laplace_tier('abb') THEN
  RAISE EXCEPTION 'Native entity projection used child physical attribute positions';
 END IF;
 SELECT pg_get_expr(relpartbound,oid) INTO STRICT bounds FROM pg_class WHERE oid='renamed_path_range'::regclass;
 ALTER TABLE physicality DETACH PARTITION renamed_path_range;
 CREATE TABLE reordered_path_range AS SELECT mask,path,hilbert,tier,entity FROM renamed_path_range;
 ALTER TABLE reordered_path_range ALTER COLUMN entity SET NOT NULL,
   ALTER COLUMN tier SET NOT NULL,ALTER COLUMN path SET NOT NULL,
   ALTER COLUMN hilbert SET NOT NULL,ALTER COLUMN mask SET NOT NULL,ADD PRIMARY KEY(entity),
   ADD CONSTRAINT physicality_mask_valid CHECK(laplace_mask_valid(mask));
 EXECUTE 'ALTER TABLE physicality ATTACH PARTITION reordered_path_range '||bounds;
 ALTER TABLE physicality ADD CONSTRAINT physicality_entity_fkey FOREIGN KEY(entity) REFERENCES entity(id);
 IF (SELECT count(*) FROM laplace_physicalities(ARRAY[laplace_id('abb')]))<>1
 OR (SELECT count(*) FROM laplace_holders(ARRAY[laplace_cp_id(97)],'{}',true))<>1
 OR (SELECT count(*) FROM laplace_containers(ARRAY[laplace_cp_id(97)],'{}'::smallint[]))<>1 THEN
  RAISE EXCEPTION 'Native physicality projection used child physical attribute positions';
 END IF;
 UPDATE physicality SET path=ST_SetPoint(path,0,
   ST_MakePoint(ST_X(ST_PointN(path,1)),ST_Y(ST_PointN(path,1)),ST_Z(ST_PointN(path,1)),1073741825))
 WHERE entity=laplace_id('abb');
 IF (SELECT count(*) FROM laplace_attested(ARRAY[laplace_cp_id(97)]))<>1 THEN
  RAISE EXCEPTION 'Native assertion index used parent physical attribute positions';
 END IF;
END $$;
ROLLBACK;

BEGIN;
SET LOCAL work_mem='64kB';
-- One real wide composition spans multiple input windows. Every chunk's GIN
-- nominations must still be checked against the complete constituent set.
CREATE TEMP TABLE wide_container AS
SELECT array_agg(laplace_cp_id(i) ORDER BY i) ids FROM generate_series(10000,11999) i;
INSERT INTO entity(id,tier,coord,hilbert)
SELECT laplace_compose(ids),1,coord,laplace_hilbert4(coord)
FROM wide_container CROSS JOIN LATERAL (
 SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(ST_GeomFromEWKB(laplace_cp_coord_ewkb(laplace_codepoint(p))))) coord
 FROM unnest(ids) p
) c;
INSERT INTO physicality(entity,tier,hilbert,path)
SELECT laplace_compose(w.ids),e.tier,e.hilbert,laplace_path(w.ids)
FROM wide_container w JOIN entity e ON e.id=laplace_compose(w.ids);
DO $$ DECLARE ids blake3[]:=(SELECT w.ids FROM wide_container w); BEGIN
 IF (SELECT count(*) FROM laplace_containers(ids,'{}'::smallint[]))<>1
 OR (SELECT count(*) FROM laplace_holders(ids))<>1
 OR (SELECT count(*) FROM laplace_holders(ids,'{}',true))<>cardinality(ids)
 OR EXISTS(SELECT FROM laplace_containers(ids||ARRAY[laplace_cp_id(12000)],'{}'::smallint[])) THEN
  RAISE EXCEPTION 'windowed all-key containment lost a complete match or admitted a partial match';
 END IF;
END $$;
ROLLBACK;
