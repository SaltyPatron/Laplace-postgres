\echo Use "ALTER EXTENSION laplace UPDATE TO '1.34'" to load this file. \quit

CREATE FUNCTION laplace_clean_stage(paths regclass) RETURNS bigint
  AS 'MODULE_PATHNAME', 'laplace_clean_stage'
  LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SET search_path = pg_catalog, public, pg_temp;
COMMENT ON FUNCTION laplace_clean_stage(regclass) IS
  'Drain GIN pending entries for copied path IDs, routing through Native partition bounds and the existing restricted maintenance authority. Returns cleaned pending pages; no input cutoff or public path scan.';

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
  -- COPY inputs have no inherited statistics. The complete set joins and
  -- groups on these IDs. Geometry histograms cannot help those joins, but
  -- PostGIS would reconstruct and sample packed paths on every merge.
  EXECUTE format('ANALYZE %s (id), %s (entity)', e, p);
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

  updated_rows := laplace_enrich_stage(entities, 'public.entity'::regclass);

  EXECUTE format('INSERT INTO public.physicality (entity, tier, hilbert, path, mask)
    SELECT s.entity, s.tier, s.hilbert, s.path, s.mask FROM %s s
    WHERE NOT EXISTS (SELECT FROM public.physicality d WHERE d.entity = s.entity)
    ORDER BY s.hilbert, s.entity', p);
  GET DIAGNOSTICS physicality_rows = ROW_COUNT;

  changed := laplace_enrich_stage(paths, 'public.physicality'::regclass);
  updated_rows := updated_rows + changed;
  -- Drain pending entries before the next provenance/series read.
  IF physicality_rows > 0 OR changed > 0 THEN
    PERFORM laplace_clean_stage(paths);
  END IF;
  -- Only this caller's copied input is consumed. A rollback restores both the
  -- public changes and its input, so an interrupted batch can be replayed.
  EXECUTE format('TRUNCATE %s, %s', e, p);
END $$;

