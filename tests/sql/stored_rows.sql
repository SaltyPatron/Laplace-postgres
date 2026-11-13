\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
-- Request positions, exact content and access policy are shared by every
-- client of the native set reader; partition names are not part of its API.
-- The database is shared: leaves are IDs no other content holds, and every
-- statement is rolled back.
BEGIN;
CREATE FUNCTION pg_temp.leaf(n integer) RETURNS blake3 LANGUAGE sql IMMUTABLE
RETURN md5('Laplace check regression stored_rows leaf '||n)::blake3;
INSERT INTO entity(id,tier,coord,hilbert)
SELECT laplace_compose(ARRAY[pg_temp.leaf(97),pg_temp.leaf(98),pg_temp.leaf(98)]),laplace_tier('abb'),
       laplace_coord('abb'),laplace_hilbert('abb');
INSERT INTO physicality(entity,tier,hilbert,path)
SELECT id,tier,hilbert,laplace_path(ARRAY[pg_temp.leaf(97),pg_temp.leaf(98),pg_temp.leaf(98)])
FROM entity WHERE id=laplace_compose(ARRAY[pg_temp.leaf(97),pg_temp.leaf(98),pg_temp.leaf(98)]);
DO $$
DECLARE known blake3:=laplace_compose(ARRAY[pg_temp.leaf(97),pg_temp.leaf(98),pg_temp.leaf(98)]);
        missing blake3:='00000000000000000000000000000000';
        requested blake3[];
BEGIN
 requested:=ARRAY[known,missing,known];
 IF laplace_id('abb') IS DISTINCT FROM laplace_compose(ARRAY[laplace_cp_id(97),laplace_cp_id(98),laplace_cp_id(98)]) THEN
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
 -- The leaves have no entity rows. Their missing rows must not turn the
 -- parent's tier predicate into NULL and hide the whole frontier.
 IF (SELECT count(*) FROM laplace_containers(ARRAY[pg_temp.leaf(97),pg_temp.leaf(98)],'{}'::smallint[]))<>1
 OR (SELECT count(*) FROM laplace_containers(ARRAY[pg_temp.leaf(97),pg_temp.leaf(97),pg_temp.leaf(98)],'{}'::smallint[]))<>1
 OR EXISTS(SELECT FROM laplace_containers(ARRAY[pg_temp.leaf(97),pg_temp.leaf(99)],'{}'::smallint[]))
 OR EXISTS(SELECT FROM laplace_containers(ARRAY[]::blake3[],'{}'::smallint[]))
 OR EXISTS(SELECT FROM laplace_containers(ARRAY[pg_temp.leaf(97)],ARRAY[laplace_bank_bit('kind','claim')])) THEN
  RAISE EXCEPTION 'Native all-key containment lost the virtual floor, duplicates or mask restriction';
 END IF;
 UPDATE physicality SET mask=laplace_mask_slot('kind','claim') WHERE entity=known;
 IF (SELECT count(*) FROM laplace_containers(ARRAY[pg_temp.leaf(97)],ARRAY[laplace_bank_bit('kind','claim')]))<>1 THEN
  RAISE EXCEPTION 'Native container carrier mask was not applied';
 END IF;
END $$;
DO $$ DECLARE reader text:='laplace_check_stored_reader_'||pg_backend_pid(); denied boolean:=false;
  known blake3:=laplace_compose(ARRAY[pg_temp.leaf(97),pg_temp.leaf(98),pg_temp.leaf(98)]);
  a blake3:=pg_temp.leaf(97);
BEGIN
 EXECUTE format('CREATE ROLE %I',reader);
 EXECUTE format('GRANT USAGE ON SCHEMA public TO %I',reader);
 EXECUTE format('GRANT SELECT(id,tier,coord,hilbert,mask) ON entity TO %I',reader);
 EXECUTE format('GRANT SELECT(entity,tier,hilbert,path,mask) ON physicality TO %I',reader);
 EXECUTE format('SET LOCAL ROLE %I',reader);
 IF (SELECT count(*) FROM laplace_entities(ARRAY[known]))<>1
 OR (SELECT count(*) FROM laplace_physicalities(ARRAY[known]))<>1
 OR (SELECT count(*) FROM laplace_containers(ARRAY[a],'{}'::smallint[]))<>1 THEN
  RAISE EXCEPTION 'column-authorized reader lost stored rows';
 END IF;
 RESET ROLE;
 EXECUTE format('REVOKE SELECT(coord) ON entity FROM %I',reader);
 EXECUTE format('SET LOCAL ROLE %I',reader);
 BEGIN
  PERFORM laplace_entities(ARRAY[known]);
 EXCEPTION WHEN insufficient_privilege THEN denied:=true;
 END;
 RESET ROLE;
 IF NOT denied THEN RAISE EXCEPTION 'stored-row reader bypassed column ACL'; END IF;
END $$;
-- A claim vertex is nominated through the assertion index.
DO $$ DECLARE known blake3:=laplace_compose(ARRAY[pg_temp.leaf(97),pg_temp.leaf(98),pg_temp.leaf(98)]); BEGIN
 UPDATE physicality SET path=ST_SetPoint(path,0,
   ST_MakePoint(ST_X(ST_PointN(path,1)),ST_Y(ST_PointN(path,1)),ST_Z(ST_PointN(path,1)),1073741825))
 WHERE entity=known;
 IF (SELECT count(*) FROM laplace_attested(ARRAY[pg_temp.leaf(97)]))<>1 THEN
  RAISE EXCEPTION 'Native assertion index lost a claim vertex';
 END IF;
END $$;
ROLLBACK;

BEGIN;
SET LOCAL work_mem='64kB';
CREATE FUNCTION pg_temp.leaf(n integer) RETURNS blake3 LANGUAGE sql IMMUTABLE
RETURN md5('Laplace check regression stored_rows leaf '||n)::blake3;
-- One wide composition spans multiple input windows. Every chunk's GIN
-- nominations must still be checked against the complete constituent set.
CREATE TEMP TABLE wide_container AS
SELECT array_agg(pg_temp.leaf(i) ORDER BY i) ids FROM generate_series(10000,11999) i;
INSERT INTO entity(id,tier,coord,hilbert)
SELECT laplace_compose(ids),1,coord,laplace_hilbert4(coord)
FROM wide_container CROSS JOIN LATERAL (
 SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(ST_GeomFromEWKB(laplace_cp_coord_ewkb(i)))) coord
 FROM generate_series(10000,11999) i
) c;
INSERT INTO physicality(entity,tier,hilbert,path)
SELECT laplace_compose(w.ids),e.tier,e.hilbert,laplace_path(w.ids)
FROM wide_container w JOIN entity e ON e.id=laplace_compose(w.ids);
DO $$ DECLARE ids blake3[]:=(SELECT w.ids FROM wide_container w); BEGIN
 IF (SELECT count(*) FROM laplace_containers(ids,'{}'::smallint[]))<>1
 OR (SELECT count(*) FROM laplace_holders(ids))<>1
 OR (SELECT count(*) FROM laplace_holders(ids,'{}',true))<>cardinality(ids)
 OR EXISTS(SELECT FROM laplace_containers(ids||ARRAY[pg_temp.leaf(12000)],'{}'::smallint[])) THEN
  RAISE EXCEPTION 'windowed all-key containment lost a complete match or admitted a partial match';
 END IF;
END $$;
ROLLBACK;
