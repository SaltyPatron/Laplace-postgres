\echo Use "ALTER EXTENSION laplace UPDATE TO '1.25'" to load this file. \quit

-- Missing type annotations may be enriched over exactly the same content.
-- Conflicting annotations, outcomes, positions and scores remain conflicts.
CREATE FUNCTION laplace_path_enrich_ewkb(recorded geometry, incoming geometry)
RETURNS bytea AS 'MODULE_PATHNAME', 'laplace_path_enrich_ewkb'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_path_enrich(recorded geometry, incoming geometry)
RETURNS geometry LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
BEGIN ATOMIC
  SELECT ST_SetSRID(ST_GeomFromEWKB(laplace_path_enrich_ewkb(recorded,incoming)),ST_SRID(recorded))
  WHERE ST_SRID(recorded)=ST_SRID(incoming);
END;

CREATE OR REPLACE FUNCTION laplace_merge_stage(entities regclass, paths regclass,
  OUT entity_rows bigint, OUT physicality_rows bigint, OUT updated_rows bigint)
RETURNS record LANGUAGE plpgsql VOLATILE PARALLEL UNSAFE AS $$
DECLARE e text; p text; invalid boolean; changed bigint;
BEGIN
  SELECT format('%I.%I', n.nspname, c.relname) INTO e
  FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
  WHERE c.oid = entities AND n.nspname = 'stage' AND c.relpersistence = 'u' AND c.relkind = 'r';
  SELECT format('%I.%I', n.nspname, c.relname) INTO p
  FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
  WHERE c.oid = paths AND n.nspname = 'stage' AND c.relpersistence = 'u' AND c.relkind = 'r';
  IF e IS NULL OR p IS NULL OR entities = paths THEN
    RAISE EXCEPTION 'laplace_merge_stage requires distinct UNLOGGED stage input tables' USING ERRCODE = '22023';
  END IF;

  -- Validation/anti-join/insertion share this lock and statement snapshots.
  -- Competing batches cannot both decide that the same public ID is absent.
  PERFORM pg_advisory_xact_lock(1279348803, -2);
  -- COPY inputs have no inherited statistics. Plan the complete set, rather
  -- than favoring EXISTS startup plans with per-ID partition probes.
  EXECUTE format('ANALYZE %s, %s', e, p);
  EXECUTE format('SELECT EXISTS (SELECT FROM %1$s GROUP BY id HAVING count(*) <> 1)
    OR EXISTS (SELECT FROM %2$s GROUP BY entity HAVING count(*) <> 1)
    OR EXISTS (SELECT FROM %1$s WHERE tier NOT BETWEEN 1 AND 255 OR NOT laplace_mask_valid(mask))
    OR EXISTS (SELECT FROM %2$s WHERE tier NOT BETWEEN 1 AND 255 OR NOT laplace_mask_valid(mask))
    OR (SELECT count(*) > 0 FROM %1$s s JOIN public.entity d USING (id)
               WHERE ST_AsEWKB(s.coord) <> ST_AsEWKB(d.coord) OR s.hilbert <> d.hilbert)
    OR (SELECT count(*) > 0 FROM %2$s s JOIN public.physicality d ON d.entity = s.entity
               WHERE (ST_AsEWKB(s.path) <> ST_AsEWKB(d.path) AND laplace_path_enrich(d.path,s.path) IS NULL) OR s.hilbert <> d.hilbert)
    OR (SELECT count(*) > 0 FROM %1$s s LEFT JOIN %2$s batch ON batch.entity = s.id
               LEFT JOIN public.physicality d ON d.entity = s.id WHERE batch.entity IS NULL AND d.entity IS NULL)
    OR (SELECT count(*) > 0 FROM %2$s s LEFT JOIN %1$s batch ON batch.id = s.entity
               LEFT JOIN public.entity d ON d.id = s.entity WHERE batch.id IS NULL AND d.id IS NULL)', e, p)
    INTO invalid;
  IF invalid THEN
    RAISE EXCEPTION 'laplace_merge_stage: duplicate IDs, invalid carriers, conflicting identity data or unpaired entities/paths'
      USING ERRCODE = '23514';
  END IF;

  EXECUTE format('INSERT INTO public.entity (id, tier, coord, hilbert, mask)
    SELECT s.id, s.tier, s.coord, s.hilbert, s.mask FROM %s s
    WHERE NOT EXISTS (SELECT FROM public.entity d WHERE d.id = s.id)
    ORDER BY s.hilbert, s.id', e);
  GET DIAGNOSTICS entity_rows = ROW_COUNT;

  EXECUTE format('UPDATE public.entity d SET tier = least(d.tier, s.tier),
    mask = laplace_mask_merge(d.mask, s.mask) FROM %s s WHERE d.id = s.id
    AND (d.tier > s.tier OR (d.mask | s.mask) <> d.mask)', e);
  GET DIAGNOSTICS updated_rows = ROW_COUNT;

  EXECUTE format('INSERT INTO public.physicality (entity, tier, hilbert, path, mask)
    SELECT s.entity, s.tier, s.hilbert, s.path, s.mask FROM %s s
    WHERE NOT EXISTS (SELECT FROM public.physicality d WHERE d.entity = s.entity)
    ORDER BY s.hilbert, s.entity', p);
  GET DIAGNOSTICS physicality_rows = ROW_COUNT;

  EXECUTE format('UPDATE public.physicality d SET tier = least(d.tier, s.tier),
    path = CASE WHEN ST_AsEWKB(d.path) <> ST_AsEWKB(s.path) THEN laplace_path_enrich(d.path,s.path) ELSE d.path END,
    mask = laplace_mask_merge(d.mask, s.mask) FROM %s s WHERE d.entity = s.entity
    AND (d.tier > s.tier OR (d.mask | s.mask) <> d.mask
      OR (ST_AsEWKB(d.path) <> ST_AsEWKB(s.path) AND ST_AsEWKB(laplace_path_enrich(d.path,s.path)) <> ST_AsEWKB(d.path)))', p);
  GET DIAGNOSTICS changed = ROW_COUNT;
  updated_rows := updated_rows + changed;
  -- Only this caller's copied input is consumed. A rollback restores both the
  -- public changes and its input, so an interrupted batch can be replayed.
  EXECUTE format('TRUNCATE %s, %s', e, p);
END $$;

