\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
\ir geometry_cases.sql
-- Wide, repeated paths exercise the allocation bound and deduplication while
-- endpoints still follow occurrence order, rather than sorted membership.
INSERT INTO geometry_cases
SELECT 'wide-repeated',laplace_path(array_agg(laplace_cp_id(97+(i%8)) ORDER BY i)),
       ARRAY(SELECT laplace_cp_id(k) FROM generate_series(97,104) k)
FROM generate_series(0,4096) i;
CREATE INDEX geometry_cases_gin ON geometry_cases USING gin(path laplace_path_ops);
DO $$ BEGIN
 BEGIN
  INSERT INTO geometry_cases VALUES('invalid-xy',ST_GeomFromText('POINT(1 2)'),'{}'::blake3[]);
  RAISE EXCEPTION 'GIN extraction accepted a geometry missing ZM';
 EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
DO $$
DECLARE r record; wanted blake3; sequential text[]; indexed text[]; plan json; expression text;
BEGIN
  FOR wanted IN SELECT laplace_cp_id(i) FROM generate_series(96,105) i LOOP
    FOREACH expression IN ARRAY ARRAY['path @> ARRAY[$1]', 'path && ARRAY[$1]', 'path ^@ $1', 'path @^ $1'] LOOP
      PERFORM set_config('enable_seqscan', 'on', true);
      PERFORM set_config('enable_bitmapscan', 'off', true);
      PERFORM set_config('enable_indexscan', 'off', true);
      EXECUTE 'SELECT array_agg(name ORDER BY name) FROM geometry_cases WHERE ' || expression INTO sequential USING wanted;
      PERFORM set_config('enable_seqscan', 'off', true);
      PERFORM set_config('enable_bitmapscan', 'on', true);
      PERFORM set_config('enable_indexscan', 'on', true);
      EXECUTE 'SELECT array_agg(name ORDER BY name) FROM geometry_cases WHERE ' || expression INTO indexed USING wanted;
      IF indexed IS DISTINCT FROM sequential THEN RAISE EXCEPTION 'indexed % differs for %', expression, wanted; END IF;
      EXECUTE 'EXPLAIN (FORMAT JSON) SELECT name FROM geometry_cases WHERE ' || expression INTO plan USING wanted;
      IF plan::text NOT LIKE '%Bitmap Index Scan%' THEN RAISE EXCEPTION '% did not use GIN: %', expression, plan; END IF;
    END LOOP;
  END LOOP;
  FOR r IN SELECT * FROM geometry_cases LOOP
    IF NOT r.path @> r.expected THEN RAISE EXCEPTION 'n-ary containment failed for %', r.name; END IF;
  END LOOP;
  IF (SELECT count(*) FROM geometry_cases WHERE name = 'strand-8' AND path ^@ laplace_cp_id(98)) <> 0
     OR (SELECT count(*) FROM geometry_cases WHERE name = 'strand-8' AND path @^ laplace_cp_id(98)) <> 0
     OR (SELECT count(*) FROM geometry_cases WHERE name = 'strand-8' AND path ^@ laplace_cp_id(97) AND path @^ laplace_cp_id(104)) <> 1
     OR (SELECT count(*) FROM geometry_cases WHERE name = 'runs' AND path ^@ laplace_cp_id(97) AND path @^ laplace_cp_id(97)) <> 1 THEN
    RAISE EXCEPTION 'endpoint roles were confused with interior membership';
  END IF;
END $$;
SELECT 'gin_path: all-vertex and endpoint GIN postings match sequential scans for every ZM family';

-- A broad frontier can make a sequential scan cheaper than thousands of GIN
-- probes. Its stable search array must not be rebuilt and scanned for every
-- stored path. Exercise the same external Param used by Native provenance.
CREATE TEMP TABLE path_search_cases AS
SELECT path FROM geometry_cases CROSS JOIN generate_series(1,1000);
SET statement_timeout = '5s';
DO $$
DECLARE frontier blake3[] := ARRAY(SELECT md5(i::text)::blake3 FROM generate_series(1,120000) i);
        wanted bigint; actual bigint; varying_wanted bigint;
BEGIN
  SELECT count(*) INTO wanted FROM path_search_cases WHERE path @> ARRAY[laplace_cp_id(97)];
  EXECUTE 'SELECT count(*) FROM path_search_cases WHERE path && $1'
    INTO actual USING frontier || ARRAY[laplace_cp_id(97),laplace_cp_id(97)];
  IF actual <> wanted THEN RAISE EXCEPTION 'large frontier lost exact membership'; END IF;
  EXECUTE 'SELECT count(*) FROM path_search_cases WHERE path && $1' INTO actual USING frontier;
  IF actual <> 0 THEN RAISE EXCEPTION 'absent frontier matched a path'; END IF;
  EXECUTE 'SELECT count(*) FROM path_search_cases WHERE path @> $1'
    INTO actual USING ARRAY[laplace_cp_id(97),laplace_cp_id(97)];
  IF actual <> wanted THEN RAISE EXCEPTION 'duplicate query keys changed containment'; END IF;
  EXECUTE 'SELECT count(*) FROM path_search_cases WHERE path && $1' INTO actual USING '{}'::blake3[];
  IF actual <> 0 THEN RAISE EXCEPTION 'empty overlap query matched a path'; END IF;
  EXECUTE 'SELECT count(*) FROM path_search_cases WHERE path @> $1' INTO actual USING '{}'::blake3[];
  IF actual <> (SELECT count(*) FROM path_search_cases) THEN RAISE EXCEPTION 'empty containment query lost a path'; END IF;

  -- Row-dependent right operands cannot share a query-wide cache.
  SELECT 2 * count(*) INTO varying_wanted FROM geometry_cases
    WHERE path && ARRAY[laplace_cp_id(97)];
  SELECT varying_wanted + 2 * count(*) INTO varying_wanted FROM geometry_cases
    WHERE path && ARRAY[laplace_cp_id(105)];
  SELECT count(*) INTO actual FROM geometry_cases CROSS JOIN generate_series(0,3) i
    WHERE path && CASE WHEN i % 2 = 0 THEN ARRAY[laplace_cp_id(97)] ELSE ARRAY[laplace_cp_id(105)] END;
  IF actual <> varying_wanted THEN RAISE EXCEPTION 'varying search arrays reused stale keys'; END IF;
END $$;
RESET statement_timeout;
SELECT 'gin_path: broad stable frontiers and varying arrays preserve exact membership';

CREATE INDEX path_search_cases_gin ON path_search_cases USING gin(path laplace_path_ops) WITH (fastupdate=on);
-- This path remains in the pending list; the access method must handle it as
-- well as the built posting lists while Native supplies exact set membership.
INSERT INTO path_search_cases VALUES(laplace_path(ARRAY[laplace_cp_id(9731),laplace_cp_id(9733)]));
SET statement_timeout='5s';
DO $$ DECLARE frontier blake3[]:=ARRAY(SELECT md5(i::text)::blake3 FROM generate_series(1,120000) i);
 expected bigint; found bigint; plan json; BEGIN
 SELECT count(*) INTO expected FROM path_search_cases WHERE path && ARRAY[laplace_cp_id(97),laplace_cp_id(9731)];
 FOR i IN 1..3 LOOP
  EXECUTE 'SELECT count(*) FROM path_search_cases WHERE path && $1' INTO found
   USING frontier||ARRAY[laplace_cp_id(97),laplace_cp_id(9731),laplace_cp_id(97)];
  IF found<>expected THEN RAISE EXCEPTION 'streamed GIN set lost built/pending postings: % versus %',found,expected; END IF;
  EXECUTE 'SELECT count(*) FROM path_search_cases WHERE path && $1' INTO found USING frontier;
  IF found<>0 THEN RAISE EXCEPTION 'streamed GIN set reused stale membership'; END IF;
 END LOOP;
 EXECUTE 'EXPLAIN (FORMAT JSON) SELECT count(*) FROM path_search_cases WHERE path && $1' INTO plan
  USING frontier||ARRAY[laplace_cp_id(9731)];
 IF plan::text NOT LIKE '%Bitmap Index Scan%' THEN RAISE EXCEPTION 'streamed set did not use GIN: %',plan; END IF;
END $$;
RESET statement_timeout;
SELECT 'gin_path: streamed ID sets preserve built and pending postings across repeated scans';
