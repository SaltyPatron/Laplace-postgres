\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
\ir geometry_cases.sql
CREATE INDEX geometry_cases_gin ON geometry_cases USING gin(path laplace_path_ops);
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
