\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
-- The deployed database holds real content. laplace_recoordinate audits and
-- repairs every stored row and has no scope, so this case exercises its Native
-- kernel, laplace_project_path, on this case's own paths and reads no table.
BEGIN;
CREATE TEMP TABLE projection_expected AS
SELECT laplace_id(word) AS id, laplace_tier(word) AS tier,
  laplace_coord(word) AS coord, laplace_hilbert(word) AS hilbert,
  laplace_path(ARRAY(SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table(word,'') c)) AS path
FROM unnest(ARRAY['dog','cat','ant']) word;
INSERT INTO projection_expected
SELECT laplace_compose(parts),greatest(laplace_tier('dog'),laplace_tier('cat'))+1,point,
  laplace_hilbert4(point) # '-9223372036854775808'::bigint,laplace_path(parts)
FROM (SELECT ARRAY[laplace_id('dog'),laplace_id('dog'),laplace_id('cat')] AS parts,
  ST_GeomFromEWKB((SELECT laplace_centroid4d_ewkb(p) FROM unnest(ARRAY[
    laplace_coord('dog'),laplace_coord('dog'),laplace_coord('cat')]) p)) AS point) composed;
DO $$ DECLARE projected record; x record; BEGIN
  IF NOT EXISTS(SELECT FROM projection_expected WHERE ST_NPoints(path)=2 AND laplace_path_length(path)=3) THEN
    RAISE EXCEPTION 'fixture did not retain actual run-compressed path';
  END IF;
  -- Words project from the floor alone; the composition is run weighted
  -- from its children's supplied coordinates.
  FOR x IN SELECT * FROM projection_expected LOOP
    IF ST_NPoints(x.path)=2 THEN
      SELECT * INTO projected FROM laplace_project_path(x.id,x.path,
        ARRAY[laplace_id('dog'),laplace_id('cat')],ARRAY[laplace_coord('dog'),laplace_coord('cat')]);
    ELSE
      SELECT * INTO projected FROM laplace_project_path(x.id,x.path,'{}'::blake3[],'{}'::geometry[]);
    END IF;
    IF projected.coord_ewkb<>ST_AsEWKB(x.coord) OR projected.hilbert<>x.hilbert THEN
      RAISE EXCEPTION 'Native projection missed the exact run-weighted coordinate or Hilbert key';
    END IF;
  END LOOP;
  BEGIN
    PERFORM laplace_project_path(laplace_compose(ARRAY[laplace_id('dog'),laplace_id('dog'),laplace_id('cat')]),
      laplace_path(ARRAY[laplace_id('dog'),laplace_id('cat'),laplace_id('cat')]),
      ARRAY[laplace_id('dog'),laplace_id('cat')],ARRAY[laplace_coord('dog'),laplace_coord('cat')]);
    RAISE EXCEPTION 'invalid stored identity accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
  BEGIN
    PERFORM laplace_project_path(laplace_compose(ARRAY[laplace_id('dog'),laplace_id('unrecorded projection child')]),
      laplace_path(ARRAY[laplace_id('dog'),laplace_id('unrecorded projection child')]),
      ARRAY[laplace_id('dog')],ARRAY[laplace_coord('dog')]);
    RAISE EXCEPTION 'missing child accepted';
  EXCEPTION WHEN foreign_key_violation THEN NULL; END;
  BEGIN
    PERFORM laplace_project_path(laplace_compose(ARRAY[laplace_id('dog'),laplace_id('cat')]),
      laplace_path(ARRAY[laplace_id('dog'),laplace_id('cat')]),ARRAY[laplace_id('dog'),laplace_id('cat')],
      ARRAY[ST_MakePoint(2,0,0,0),laplace_coord('cat')]);
    RAISE EXCEPTION 'out-of-wall child accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
  BEGIN
    PERFORM laplace_project_path(laplace_compose(ARRAY[laplace_id('dog'),laplace_id('cat')]),
      laplace_path(ARRAY[laplace_id('dog'),laplace_id('cat')]),ARRAY[laplace_id('dog'),laplace_id('cat')],
      ARRAY[ST_MakePoint(0.1,0,0,0),laplace_coord('cat')]);
    RAISE EXCEPTION 'off-grid child accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
  BEGIN
    PERFORM laplace_project_path(laplace_id('dog'),laplace_path(ARRAY[laplace_cp_id(100),laplace_cp_id(111),laplace_cp_id(103)]),
      ARRAY[laplace_cp_id(100),laplace_cp_id(100)],ARRAY[NULL::geometry,NULL::geometry]);
    RAISE EXCEPTION 'duplicate child bindings accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
-- Canonical atoms project from the floor, whatever coordinate a stored row
-- supplies; a self-path that is not exactly the atom is rejected.
CREATE TEMP TABLE projection_floor AS
SELECT id,coord,laplace_hilbert4(coord) # '-9223372036854775808'::bigint AS hilbert
FROM (SELECT laplace_cp_id(cp) AS id,ST_GeomFromEWKB(laplace_cp_coord_ewkb(cp)) AS coord
      FROM unnest(ARRAY[100,55295,55296,57343,57344,1114111]) cp) codepoints;
DO $$ DECLARE f record; projected record; BEGIN
  FOR f IN SELECT * FROM projection_floor LOOP
    SELECT * INTO projected FROM laplace_project_path(f.id,laplace_path(ARRAY[f.id]),ARRAY[f.id],ARRAY[ST_MakePoint(0,0,0,0)]);
    IF projected.coord_ewkb<>ST_AsEWKB(f.coord) OR projected.hilbert<>f.hilbert THEN
      RAISE EXCEPTION 'stored canonical atom was not projected from the floor';
    END IF;
    BEGIN
      PERFORM laplace_project_path(f.id,laplace_path(ARRAY[f.id,f.id]),'{}'::blake3[],'{}'::geometry[]);
      RAISE EXCEPTION 'run-compressed composition accepted as an atom';
    EXCEPTION WHEN check_violation THEN NULL; END;
    BEGIN
      PERFORM laplace_project_path(f.id,laplace_path(ARRAY[laplace_cp_id(120)]),'{}'::blake3[],'{}'::geometry[]);
      RAISE EXCEPTION 'another scalar accepted as an atomic self-path';
    EXCEPTION WHEN check_violation THEN NULL; END;
  END LOOP;
END $$;
ROLLBACK;
SELECT 'projection: Native run-weighted identity, child bindings, wall, grid and stored-identity rejection passed';
SELECT 'projection floor: canonical atoms project from the floor; non-atomic self-paths rejected';
