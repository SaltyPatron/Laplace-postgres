\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
-- The deployed database is shared: the fixture rows are written inside this
-- transaction and rolled back. A stored row with the same identity has the
-- same constituents and coordinate, so it is kept as it is.
BEGIN;
CREATE TEMP TABLE curve_fixture AS
SELECT laplace_id(word) AS id, laplace_tier(word) AS tier, laplace_coord(word) AS coord,
  laplace_hilbert(word) AS hilbert,
  laplace_path(ARRAY(SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table(word,'') WITH ORDINALITY t(c,n) ORDER BY n)) AS path
FROM unnest(ARRAY['dog','cat']) word;
INSERT INTO curve_fixture
SELECT laplace_compose(parts),greatest(laplace_tier('dog'),laplace_tier('cat'))+1,point,
  laplace_hilbert4(point) # '-9223372036854775808'::bigint,laplace_path(parts)
FROM (SELECT ARRAY[laplace_id('dog'),laplace_id('dog'),laplace_id('cat')] AS parts,
  ST_GeomFromEWKB((SELECT laplace_centroid4d_ewkb(p) FROM unnest(ARRAY[
    laplace_coord('dog'),laplace_coord('dog'),laplace_coord('cat')]) p)) AS point) composed;
INSERT INTO entity(id,tier,coord,hilbert) SELECT id,tier,coord,hilbert FROM curve_fixture ON CONFLICT DO NOTHING;
INSERT INTO physicality(entity,tier,hilbert,path) SELECT id,tier,hilbert,path FROM curve_fixture ON CONFLICT DO NOTHING;
CREATE FUNCTION pg_temp.cp_point(cp integer) RETURNS geometry LANGUAGE SQL IMMUTABLE
BEGIN ATOMIC SELECT ST_GeomFromEWKB(laplace_cp_coord_ewkb(cp)); END;
DO $$ DECLARE
  dogdogcat blake3:=laplace_compose(ARRAY[laplace_id('dog'),laplace_id('dog'),laplace_id('cat')]);
  dog_line geometry:=ST_MakeLine(ARRAY[pg_temp.cp_point(100),pg_temp.cp_point(111),pg_temp.cp_point(103)]);
  cat_line geometry:=ST_MakeLine(ARRAY[pg_temp.cp_point(99),pg_temp.cp_point(97),pg_temp.cp_point(116)]);
  run_line geometry:=ST_MakeLine(ARRAY[laplace_coord('dog'),laplace_coord('dog'),laplace_coord('cat')]);
  got record; n bigint;
BEGIN
  -- A word realizes through its atoms' Tier-0 coordinates, in path order.
  SELECT * INTO got FROM laplace_curves(ARRAY[laplace_id('dog')]);
  IF got.i<>1 OR got.entity<>laplace_id('dog') OR ST_AsEWKB(ST_GeomFromEWKB(got.curve_ewkb))<>ST_AsEWKB(dog_line) THEN
    RAISE EXCEPTION 'a word did not realize its atoms'' floor coordinates in order';
  END IF;
  -- A composition realizes through its children's entity coordinates, every
  -- repetition of a run written out; the packed carrier is not the curve.
  SELECT * INTO got FROM laplace_curves(ARRAY[dogdogcat]);
  IF ST_AsEWKB(ST_GeomFromEWKB(got.curve_ewkb))<>ST_AsEWKB(run_line) THEN
    RAISE EXCEPTION 'a run-compressed composition did not realize every repetition at its children''s coordinates';
  END IF;
  IF got.curve_ewkb=(SELECT ST_AsEWKB(path) FROM curve_fixture WHERE id=dogdogcat) THEN
    RAISE EXCEPTION 'the packed identity carrier was returned as shape';
  END IF;
  -- One row an input occurrence, in input order; an atom and an absent
  -- entity have no curve.
  SELECT count(*) INTO n FROM laplace_curves(ARRAY[laplace_id('dog'),laplace_cp_id(65),
      laplace_id('unrecorded curve probe'),laplace_id('dog')]) c
    WHERE (c.i IN (1,4) AND c.entity=laplace_id('dog') AND c.curve_ewkb IS NOT NULL)
       OR (c.i=2 AND c.entity=laplace_cp_id(65) AND c.curve_ewkb IS NULL)
       OR (c.i=3 AND c.curve_ewkb IS NULL);
  IF n<>4 THEN
    RAISE EXCEPTION 'occurrences, atoms or absent entities were not answered one row each';
  END IF;
  -- The measures are given realized curves: identical shapes are 0 apart, and
  -- the distance is the measure of the two realized curves.
  IF laplace_frechet4d((SELECT ST_GeomFromEWKB(curve_ewkb) FROM laplace_curves(ARRAY[laplace_id('dog')])),dog_line)<>0
     OR laplace_frechet4d((SELECT ST_GeomFromEWKB(curve_ewkb) FROM laplace_curves(ARRAY[laplace_id('cat')])),
                          (SELECT ST_GeomFromEWKB(curve_ewkb) FROM laplace_curves(ARRAY[laplace_id('dog')])))
        <>laplace_frechet4d(cat_line,dog_line) THEN
    RAISE EXCEPTION 'shape measures disagree with the realized curves';
  END IF;
END $$;
ROLLBACK;
SELECT 'curves: words and runs realize at their constituents'' real coordinates; atoms, absent ids and carriers are not shape';
