\echo Use "ALTER EXTENSION laplace UPDATE TO '1.25'" to load this file. \quit

-- Existing installations may retain canonical atoms in public tables. Their
-- POINT self-path is a leaf, not a cyclic composition. Unknown legacy IDs are
-- not canonical leaves and must never become coordinate fallbacks.
CREATE OR REPLACE FUNCTION laplace_recoordinate(apply boolean DEFAULT false,
  OUT entity_rows bigint, OUT physicality_rows bigint)
RETURNS record LANGUAGE plpgsql VOLATILE PARALLEL UNSAFE AS $$
DECLARE level smallint;
BEGIN
  PERFORM pg_advisory_xact_lock(1279348803, -2);
  IF EXISTS (SELECT FROM public.entity e FULL JOIN public.physicality p ON p.entity=e.id
      WHERE e.id IS NULL OR p.entity IS NULL OR e.tier NOT BETWEEN 0 AND 255) THEN
    RAISE EXCEPTION 'laplace_recoordinate: unpaired entity/path or invalid tier' USING ERRCODE='23514';
  END IF;
  IF EXISTS (SELECT FROM public.entity e JOIN public.physicality p ON p.entity=e.id
      WHERE e.tier=0 AND (laplace_codepoint(e.id) IS NULL OR GeometryType(p.path)<>'POINT'
        OR laplace_path_length(p.path)<>1 OR laplace_vertex_ids(p.path)<>ARRAY[e.id])) THEN
    RAISE EXCEPTION 'laplace_recoordinate: tier0 row is not a canonical scalar self-path' USING ERRCODE='23514';
  END IF;
  IF EXISTS (SELECT FROM public.entity e JOIN public.physicality p ON p.entity=e.id
      CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) AS vertex(id)
      JOIN public.entity child ON child.id=vertex.id
      WHERE child.tier >= e.tier AND NOT (e.tier=0 AND vertex.id=e.id)) THEN
    RAISE EXCEPTION 'laplace_recoordinate: path violates strict child-to-parent tier ordering' USING ERRCODE='23514';
  END IF;
  CREATE TEMP TABLE laplace_coordinate_projection (
    id blake3 PRIMARY KEY, coord geometry(POINTZM), hilbert bigint NOT NULL
  ) ON COMMIT DROP;
  FOR level IN SELECT DISTINCT tier FROM public.entity ORDER BY tier LOOP
    WITH inputs AS (
      SELECT p.entity,
        array_agg(vertex.id ORDER BY vertex.id) AS ids,
        array_agg(coalesce(fixed.coord,child.coord) ORDER BY vertex.id) AS coords
      FROM public.entity e JOIN public.physicality p ON p.entity=e.id
      CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) AS vertex(id)
      LEFT JOIN public.entity child ON child.id=vertex.id
      LEFT JOIN pg_temp.laplace_coordinate_projection fixed ON fixed.id=vertex.id
      WHERE e.tier=level GROUP BY p.entity
    )
    INSERT INTO pg_temp.laplace_coordinate_projection
    SELECT i.entity, ST_GeomFromEWKB(projected.coord_ewkb), projected.hilbert
    FROM inputs i JOIN public.physicality p ON p.entity=i.entity
    JOIN public.entity e ON e.id=i.entity
    CROSS JOIN LATERAL laplace_project_path(i.entity,p.path,i.ids,i.coords) projected
    WHERE ST_AsEWKB(e.coord) <> projected.coord_ewkb OR e.hilbert <> projected.hilbert
       OR p.hilbert <> projected.hilbert;
  END LOOP;
  SELECT count(*) INTO entity_rows FROM pg_temp.laplace_coordinate_projection f
    JOIN public.entity e USING(id) WHERE ST_AsEWKB(e.coord)<>ST_AsEWKB(f.coord) OR e.hilbert<>f.hilbert;
  SELECT count(*) INTO physicality_rows FROM pg_temp.laplace_coordinate_projection f
    JOIN public.physicality p ON p.entity=f.id WHERE p.hilbert<>f.hilbert;
  IF apply THEN
    UPDATE public.entity e SET coord=f.coord,hilbert=f.hilbert FROM pg_temp.laplace_coordinate_projection f
      WHERE e.id=f.id AND (ST_AsEWKB(e.coord)<>ST_AsEWKB(f.coord) OR e.hilbert<>f.hilbert);
    UPDATE public.physicality p SET hilbert=f.hilbert FROM pg_temp.laplace_coordinate_projection f
      WHERE p.entity=f.id AND p.hilbert<>f.hilbert;
  END IF;
  DROP TABLE pg_temp.laplace_coordinate_projection;
END $$;

COMMENT ON FUNCTION laplace_recoordinate(boolean) IS
  'Explicit canonical DAG projection audit or atomic repair, including stored canonical scalar atoms; rejects unknown/non-atomic leaves and preserves content IDs, paths, masks, witnesses and consensus.';
