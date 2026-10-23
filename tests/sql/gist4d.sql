\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
SET search_path = laplace, public;
CREATE TEMP TABLE curves (id integer PRIMARY KEY, trajectory geometry);
-- Most curves have the query's box and endpoints, but visit its interior in
-- the wrong order. Their bound is zero while their exact distance is large.
INSERT INTO curves
SELECT n, CASE WHEN n <= 100 THEN
  ST_GeomFromText(format('LINESTRING ZM(0 0 0 %s,1 1 1 %s,2 -1 -1 %s,3 0 0 %s)',
    n/10000.0, 1+n/10000.0, -1+n/10000.0, n/10000.0))
ELSE ST_GeomFromText(format('LINESTRING ZM(0 0 0 0,2 -1 -1 %s,1 1 1 1,3 0 0 0)', -1+n/100000.0)) END
FROM generate_series(1,2000) n;
CREATE INDEX curves_knn ON curves USING gist(trajectory laplace_traj_ops);
CREATE TEMP TABLE points (id integer PRIMARY KEY, coord geometry);
INSERT INTO points SELECT n, ST_MakePoint(0,0,cos(n/1000.0),sin(n/1000.0)) FROM generate_series(1,2000) n;
CREATE INDEX points_knn ON points USING gist(coord laplace_coord_ops);
CREATE INDEX points_angular_knn ON points USING gist(coord laplace_angular4d_ops);
DO $$
DECLARE query geometry := ST_GeomFromText('LINESTRING ZM(0 0 0 0,1 1 1 1,2 -1 -1 -1,3 0 0 0)');
        origin geometry := ST_MakePoint(0,0,1,0); expected integer[]; actual integer[]; plan json; sql text;
BEGIN
  PERFORM set_config('enable_indexscan', 'off', true);
  SELECT array_agg(id ORDER BY distance) INTO expected FROM
    (SELECT id, laplace_frechet4d(trajectory,query) AS distance FROM curves ORDER BY distance LIMIT 10) s;
  PERFORM set_config('enable_seqscan', 'off', true);
  PERFORM set_config('enable_indexscan', 'on', true);
  SELECT array_agg(id ORDER BY distance) INTO actual FROM
    (SELECT id, trajectory <-> query AS distance FROM curves ORDER BY trajectory <-> query LIMIT 10) s;
  IF actual IS DISTINCT FROM expected OR actual <> ARRAY[1,2,3,4,5,6,7,8,9,10] THEN
    RAISE EXCEPTION 'GiST returned bounding-box order instead of exact Frechet order: % vs %', actual, expected;
  END IF;
  EXECUTE 'EXPLAIN (FORMAT JSON) SELECT id FROM curves ORDER BY trajectory <-> $1 LIMIT 10' INTO plan USING query;
  IF plan::text NOT LIKE '%curves_knn%' OR plan::text NOT LIKE '%Order By%' THEN
    RAISE EXCEPTION 'trajectory KNN index was not used: %', plan;
  END IF;
  FOREACH sql IN ARRAY ARRAY['coord <-> $1', 'coord <=> $1'] LOOP
    PERFORM set_config('enable_seqscan', 'on', true);
    PERFORM set_config('enable_indexscan', 'off', true);
    EXECUTE 'SELECT array_agg(id ORDER BY distance) FROM (SELECT id, ' || sql || ' AS distance FROM points ORDER BY distance LIMIT 10) s'
      INTO expected USING origin;
    PERFORM set_config('enable_seqscan', 'off', true);
    PERFORM set_config('enable_indexscan', 'on', true);
    EXECUTE 'SELECT array_agg(id ORDER BY distance) FROM (SELECT id, ' || sql || ' AS distance FROM points ORDER BY ' || sql || ' LIMIT 10) s'
      INTO actual USING origin;
    IF actual IS DISTINCT FROM expected OR actual <> ARRAY[1,2,3,4,5,6,7,8,9,10] THEN
      RAISE EXCEPTION '4D point or S3 angular KNN order differs: % vs %', actual, expected;
    END IF;
    EXECUTE 'EXPLAIN (FORMAT JSON) SELECT id FROM points ORDER BY ' || sql || ' LIMIT 10' INTO plan USING origin;
    IF plan::text NOT LIKE '%Index Scan%' OR plan::text NOT LIKE '%Order By%' THEN
      RAISE EXCEPTION 'point KNN index was not used: %', plan;
    END IF;
  END LOOP;
  IF ST_MakePoint(0,0,0,0) OPERATOR(public.<->) ST_MakePoint(0,0,0,1) <> 0
     OR ST_MakePoint(0,0,0,0) <-> ST_MakePoint(0,0,0,1) <> 1 THEN
    RAISE EXCEPTION 'native fourth-axis or PostGIS 2D operator semantics changed';
  END IF;
END $$;
RESET search_path;
-- Exercise an actual install into an independently owned application schema.
-- Transaction rollback restores the regression database's extension and data.
BEGIN;
DROP EXTENSION laplace CASCADE;
DO $$ BEGIN
  IF to_regnamespace('laplace') IS NULL THEN CREATE SCHEMA laplace; END IF;
END $$;
CREATE TABLE laplace.install_owner_probe (id integer PRIMARY KEY);
INSERT INTO laplace.install_owner_probe VALUES (17);
CREATE EXTENSION laplace WITH SCHEMA public;
DO $$
BEGIN
  IF (SELECT count(*) FROM laplace.install_owner_probe WHERE id=17) <> 1
     OR EXISTS (SELECT FROM pg_depend d JOIN pg_extension e ON e.oid=d.refobjid
                WHERE d.classid='pg_namespace'::regclass AND d.objid='laplace'::regnamespace
                  AND d.refclassid='pg_extension'::regclass AND d.deptype='e' AND e.extname='laplace') THEN
    RAISE EXCEPTION 'Native install changed the existing application namespace';
  END IF;
  IF (SELECT typlen FROM pg_type WHERE oid='public.blake3'::regtype) <> 16
     OR to_regtype('public.blake3[]') IS NULL
     OR ST_MakePoint(0,0,0,0) OPERATOR(laplace.<->) ST_MakePoint(0,0,0,1) <> 1 THEN
    RAISE EXCEPTION 'Native identity or 4D operator is unavailable after shared-schema install';
  END IF;
END $$;
ROLLBACK;
SELECT 'gist4d: exact Frechet LIMIT 10 recheck, fourth-axis chord and S3 angular KNN passed';
