\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
\ir geometry_cases.sql
DO $$
DECLARE r record; actual blake3[];
BEGIN
  FOR r IN SELECT * FROM geometry_cases LOOP
    actual := laplace_vertex_ids(r.path);
    IF cardinality(actual) <> cardinality(r.expected) OR NOT actual @> r.expected OR NOT r.expected @> actual THEN
      RAISE EXCEPTION 'incorrect full ZM keys for %: %', r.name, actual;
    END IF;
    IF ST_AsEWKB(ST_GeomFromEWKB(ST_AsEWKB(r.path, 'XDR'))) <> ST_AsEWKB(r.path)
       OR laplace_vertex_ids(ST_GeomFromEWKB(ST_AsEWKB(r.path, 'XDR'))) <> actual THEN
      RAISE EXCEPTION 'EWKB roundtrip changed %', r.name;
    END IF;
    IF NOT (r.path @> '{}'::blake3[]) OR (r.path && '{}'::blake3[]) THEN
      RAISE EXCEPTION 'empty containment query semantics failed for %', r.name;
    END IF;
  END LOOP;
  BEGIN
    PERFORM laplace_vertex_ids(ST_GeomFromText('POINT (1 2)'));
    RAISE EXCEPTION 'a geometry missing ZM was accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL;
  END;
END $$;
-- Prepared external parameters are stable only for one execution; values in
-- table rows are not stable. Duplicates are set membership, not extra matches.
DO $$
DECLARE ids blake3[]; actual text[]; expected text[];
BEGIN
  FOREACH ids SLICE 1 IN ARRAY ARRAY[
    ARRAY[laplace_cp_id(97),laplace_cp_id(97)],
    ARRAY[laplace_cp_id(104),laplace_cp_id(105)],
    ARRAY[laplace_cp_id(105),laplace_cp_id(105)]] LOOP
    SELECT array_agg(name ORDER BY name) INTO expected FROM geometry_cases
      WHERE laplace_vertex_ids(path) && ids;
    EXECUTE 'SELECT array_agg(name ORDER BY name) FROM geometry_cases WHERE path && $1'
      INTO actual USING ids;
    IF actual IS DISTINCT FROM expected THEN RAISE EXCEPTION 'stable overlap query changed membership'; END IF;
    SELECT array_agg(name ORDER BY name) INTO expected FROM geometry_cases
      WHERE laplace_vertex_ids(path) @> ids;
    EXECUTE 'SELECT array_agg(name ORDER BY name) FROM geometry_cases WHERE path @> $1'
      INTO actual USING ids;
    IF actual IS DISTINCT FROM expected THEN RAISE EXCEPTION 'stable containment query changed membership'; END IF;
  END LOOP;
  IF EXISTS (SELECT FROM geometry_cases g WHERE (g.path @> g.expected) IS DISTINCT FROM true) THEN
    RAISE EXCEPTION 'row-varying containment reused another row query';
  END IF;
  ids := ARRAY(SELECT laplace_cp_id(i) FROM generate_series(200,120199) i)
      || ARRAY[laplace_cp_id(97),laplace_cp_id(97)];
  EXECUTE 'SELECT array_agg(name ORDER BY name) FROM geometry_cases WHERE path && $1'
    INTO actual USING ids;
  SELECT array_agg(name ORDER BY name) INTO expected FROM geometry_cases
    WHERE laplace_vertex_ids(path) && ARRAY[laplace_cp_id(97)];
  IF actual IS DISTINCT FROM expected THEN RAISE EXCEPTION 'large stable frontier lost membership'; END IF;
END $$;
SELECT 'geometry: all seven ZM families, 2/3/8 vertices, RLE, empties and EWKB roundtrip passed';
