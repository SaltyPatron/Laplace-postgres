\echo Use "ALTER EXTENSION laplace UPDATE TO '1.19'" to load this file. \quit

-- These are column templates, not copies of all public leaf partitions. Each
-- ingest may COPY into its own UNLOGGED tables created LIKE these templates.
-- Readiness never truncates or scans another run's pending data.
CREATE FUNCTION laplace_ensure_stage() RETURNS void
LANGUAGE plpgsql VOLATILE PARALLEL UNSAFE AS $$
BEGIN
  IF to_regclass('stage.entity') IS NOT NULL
     AND to_regclass('stage.physicality') IS NOT NULL
     AND to_regclass('stage.attestation') IS NOT NULL THEN RETURN; END IF;
  PERFORM pg_advisory_xact_lock(1279348803, -1);
  CREATE SCHEMA IF NOT EXISTS stage;
  CREATE UNLOGGED TABLE IF NOT EXISTS stage.entity (LIKE public.entity INCLUDING DEFAULTS);
  CREATE UNLOGGED TABLE IF NOT EXISTS stage.physicality (LIKE public.physicality INCLUDING DEFAULTS);
  CREATE UNLOGGED TABLE IF NOT EXISTS stage.attestation (
    claim blake3 NOT NULL, witness blake3 NOT NULL, lineage blake3,
    games bigint NOT NULL CHECK (games > 0),
    score double precision NOT NULL CHECK (score BETWEEN 0 AND 1),
    trust double precision NOT NULL CHECK (trust BETWEEN -1 AND 1)
  );
END $$;

-- One ordinary transaction owns a public merge. COPY workers may fill separate
-- UNLOGGED inputs concurrently; no prepared transactions or partition export
-- is involved. The caller can keep its transaction open through source-trunk
-- registration and continuous laplace_attest_series updates.
CREATE FUNCTION laplace_merge_stage(entities regclass, paths regclass,
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
  EXECUTE format('SELECT EXISTS (SELECT FROM %1$s GROUP BY id HAVING count(*) <> 1)
    OR EXISTS (SELECT FROM %2$s GROUP BY entity HAVING count(*) <> 1)
    OR EXISTS (SELECT FROM %1$s WHERE tier NOT BETWEEN 1 AND 255 OR NOT laplace_mask_valid(mask))
    OR EXISTS (SELECT FROM %2$s WHERE tier NOT BETWEEN 1 AND 255 OR NOT laplace_mask_valid(mask))
    OR EXISTS (SELECT FROM %1$s s JOIN public.entity d USING (id)
               WHERE ST_AsEWKB(s.coord) <> ST_AsEWKB(d.coord) OR s.hilbert <> d.hilbert)
    OR EXISTS (SELECT FROM %2$s s JOIN public.physicality d ON d.entity = s.entity
               WHERE ST_AsEWKB(s.path) <> ST_AsEWKB(d.path) OR s.hilbert <> d.hilbert)
    OR EXISTS (SELECT FROM %1$s s LEFT JOIN %2$s batch ON batch.entity = s.id
               LEFT JOIN public.physicality d ON d.entity = s.id WHERE batch.entity IS NULL AND d.entity IS NULL)
    OR EXISTS (SELECT FROM %2$s s LEFT JOIN %1$s batch ON batch.id = s.entity
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
    mask = laplace_mask_merge(d.mask, s.mask) FROM %s s WHERE d.entity = s.entity
    AND (d.tier > s.tier OR (d.mask | s.mask) <> d.mask)', p);
  GET DIAGNOSTICS changed = ROW_COUNT;
  updated_rows := updated_rows + changed;
  -- Only this caller's copied input is consumed. A rollback restores both the
  -- public changes and its input, so an interrupted batch can be replayed.
  EXECUTE format('TRUNCATE %s, %s', e, p);
END $$;

COMMENT ON FUNCTION laplace_ensure_stage() IS
  'Prepare three UNLOGGED column templates once; readiness does not loop over partitions, scan pending rows, or truncate data.';
COMMENT ON FUNCTION laplace_merge_stage(regclass, regclass) IS
  'Atomically merge caller-owned UNLOGGED inputs in Hilbert order with set anti-joins, Native checked mask merging and no two-phase commit. Consumes only the supplied input tables.';
