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
SELECT 'geometry: all seven ZM families, 2/3/8 vertices, RLE, empties and EWKB roundtrip passed';
