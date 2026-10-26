\echo Use "ALTER EXTENSION laplace UPDATE TO '1.37'" to load this file. \quit

CREATE FUNCTION laplace_present_stage(input regclass, target regclass, id_column name, present_column name,
  OUT id blake3, OUT ordinal bigint) RETURNS SETOF record
  AS 'MODULE_PATHNAME','laplace_present_stage' LANGUAGE C STABLE STRICT PARALLEL UNSAFE;
CREATE FUNCTION laplace_stage_compatible(recorded_id blake3, incoming_id blake3,
  recorded geometry, incoming geometry, recorded_hilbert bigint, incoming_hilbert bigint, paths boolean)
  RETURNS boolean AS 'MODULE_PATHNAME','laplace_stage_compatible'
  LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE FUNCTION laplace_enrich_stage(input regclass, target regclass, classified regclass, selected_rows bigint)
  RETURNS bigint AS 'MODULE_PATHNAME','laplace_enrich_stage' LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE
  SET search_path=pg_catalog,public,pg_temp;

CREATE OR REPLACE FUNCTION laplace_merge_stage(entities regclass, paths regclass,
  OUT entity_rows bigint, OUT physicality_rows bigint, OUT updated_rows bigint)
RETURNS record LANGUAGE plpgsql VOLATILE PARALLEL UNSAFE SET search_path=pg_catalog,public,pg_temp AS $$
DECLARE e text; p text; plan_name text; classified text; invalid boolean;
        e_class text; p_class text; e_flag text; p_flag text; e_key text; p_key text;
        e_join text; p_join text; e_write text; p_write text;
        entity_proof text:='false'; path_proof text:='false'; changed bigint;
        known_entities bigint; known_paths bigint; candidates bigint; previous_candidates bigint;
        retried boolean:=false; conflicting_schema text; conflicting_table text; conflicting_constraint text;
BEGIN
  SELECT format('%I.%I',n.nspname,c.relname) INTO e
  FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
  WHERE c.oid=entities AND n.nspname='stage' AND c.relpersistence='u' AND c.relkind='r';
  SELECT format('%I.%I',n.nspname,c.relname) INTO p
  FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
  WHERE c.oid=paths AND n.nspname='stage' AND c.relpersistence='u' AND c.relkind='r';
  IF e IS NULL OR p IS NULL OR entities=paths THEN
    RAISE EXCEPTION 'laplace_merge_stage requires distinct UNLOGGED stage input tables' USING ERRCODE='22023';
  END IF;
  EXECUTE format('LOCK TABLE %s,%s IN SHARE MODE',e,p);
  EXECUTE format('ANALYZE %s (id),%s (entity)',e,p);
  IF EXISTS(SELECT FROM pg_attribute WHERE attrelid=entities AND attname='present' AND NOT attisdropped) THEN
    IF NOT EXISTS(SELECT FROM pg_attribute WHERE attrelid=entities AND attname='present' AND atttypid='boolean'::regtype AND NOT attisdropped) THEN
      RAISE EXCEPTION 'entity presence classification must be boolean' USING ERRCODE='22023';
    END IF;
    entity_proof:='coalesce(a.present,false)';
  END IF;
  IF EXISTS(SELECT FROM pg_attribute WHERE attrelid=paths AND attname='present' AND NOT attisdropped) THEN
    IF NOT EXISTS(SELECT FROM pg_attribute WHERE attrelid=paths AND attname='present' AND atttypid='boolean'::regtype AND NOT attisdropped) THEN
      RAISE EXCEPTION 'path presence classification must be boolean' USING ERRCODE='22023';
    END IF;
    path_proof:='coalesce(b.present,false)';
  END IF;
  -- Novel content supplies its full canonical payload. Proven existing IDs
  -- supply only mutable metadata and optional incoming path annotations.
  EXECUTE format('SELECT EXISTS(SELECT FROM %1$s GROUP BY id HAVING count(*)<>1)
    OR EXISTS(SELECT FROM %2$s GROUP BY entity HAVING count(*)<>1)
    OR EXISTS(SELECT FROM %1$s a WHERE a.id IS NULL OR a.tier IS NULL OR a.mask IS NULL
      OR a.tier NOT BETWEEN 1 AND 255 OR NOT public.laplace_mask_valid(a.mask)
      OR (NOT %3$s AND (a.coord IS NULL OR a.hilbert IS NULL)))
    OR EXISTS(SELECT FROM %2$s b WHERE b.entity IS NULL OR b.tier IS NULL OR b.mask IS NULL
      OR b.tier NOT BETWEEN 1 AND 255 OR NOT public.laplace_mask_valid(b.mask)
      OR (NOT %4$s AND (b.path IS NULL OR b.hilbert IS NULL)))
    OR EXISTS(SELECT FROM %1$s a FULL JOIN %2$s b ON b.entity=a.id WHERE a.id IS NULL OR b.entity IS NULL)',e,p,entity_proof,path_proof) INTO invalid;
  IF invalid THEN
    RAISE EXCEPTION 'laplace_merge_stage: duplicate IDs, invalid carriers or unpaired entity/path input' USING ERRCODE='23514';
  END IF;
  IF entity_proof<>'false' AND path_proof<>'false' THEN
    -- The ordinary Native writer already copied this state with each row.
    -- Consume it in place: no second key table or duplicate payload copy.
    e_class:=e; p_class:=p; e_key:='id'; p_key:='entity'; e_flag:='present'; p_flag:='present';
    e_join:=''; p_join:=''; e_write:='coalesce(s.present,false)'; p_write:=e_write;
  ELSE
    -- A raw staged import has supplied no descent classifications. Its small
    -- ID-only plan uses the same verifier and publication statements.
    plan_name:=format('lp_plan_%s_%s',entities::oid,paths::oid);
    classified:=format('stage.%I',plan_name);
    EXECUTE format('CREATE UNLOGGED TABLE %s (id public.blake3 PRIMARY KEY,entity_present boolean NOT NULL,path_present boolean NOT NULL)',classified);
    EXECUTE format('INSERT INTO %s SELECT a.id,%s,%s FROM %s a JOIN %s b ON b.entity=a.id',classified,entity_proof,path_proof,e,p);
    e_class:=classified; p_class:=classified; e_key:='id'; p_key:='id'; e_flag:='entity_present'; p_flag:='path_present';
    e_join:=format(' JOIN %s c ON c.id=s.id',classified);
    p_join:=format(' JOIN %s c ON c.id=s.entity',classified);
    e_write:='c.entity_present'; p_write:='c.path_present';
  END IF;

  LOOP
    BEGIN
      -- One flat overlap verification of claimed-new IDs, using the same
      -- Native btree membership kernel as tier descent. Positive client
      -- classifications remain the write plan and are not probed again.
      EXECUTE format('UPDATE %1$s c SET %3$I=true
        FROM public.laplace_present_stage(%1$L::regclass,''public.entity'',%2$L,%3$L) landed WHERE landed.id=c.%2$I',e_class,e_key,e_flag);
      EXECUTE format('UPDATE %1$s c SET %3$I=true
        FROM public.laplace_present_stage(%1$L::regclass,''public.physicality'',%2$L,%3$L) landed WHERE landed.id=c.%2$I',p_class,p_key,p_flag);
      EXECUTE format('SELECT (SELECT count(*) FROM %1$s WHERE %3$I),(SELECT count(*) FROM %2$s WHERE %4$I),
        (SELECT count(*) FROM %1$s WHERE %3$I IS NOT TRUE)+(SELECT count(*) FROM %2$s WHERE %4$I IS NOT TRUE)',e_class,p_class,e_flag,p_flag)
        INTO known_entities,known_paths,candidates;
      IF retried AND candidates>=previous_candidates THEN
        RAISE EXCEPTION 'concurrent publication requires a fresh transaction snapshot' USING ERRCODE='40001';
      END IF;
      previous_candidates:=candidates;
      EXECUTE format('INSERT INTO public.entity(id,tier,coord,hilbert,mask)
        SELECT s.id,s.tier,s.coord,s.hilbert,s.mask FROM %s s%s
        WHERE NOT %s ORDER BY s.hilbert,s.id',e,e_join,e_write);
      GET DIAGNOSTICS entity_rows=ROW_COUNT;
      updated_rows:=0;
      IF known_entities>0 THEN
        updated_rows:=public.laplace_enrich_stage(entities,'public.entity',e_class::regclass,known_entities);
      END IF;
      EXECUTE format('INSERT INTO public.physicality(entity,tier,hilbert,path,mask)
        SELECT s.entity,s.tier,s.hilbert,s.path,s.mask FROM %s s%s
        WHERE NOT %s ORDER BY s.hilbert,s.entity',p,p_join,p_write);
      GET DIAGNOSTICS physicality_rows=ROW_COUNT;
      changed:=0;
      IF known_paths>0 THEN
        changed:=public.laplace_enrich_stage(paths,'public.physicality',p_class::regclass,known_paths);
      END IF;
      updated_rows:=updated_rows+changed;
      IF physicality_rows>0 OR changed>0 THEN PERFORM public.laplace_clean_stage(paths); END IF;
      EXIT;
    EXCEPTION WHEN unique_violation THEN
      GET STACKED DIAGNOSTICS conflicting_schema=SCHEMA_NAME,conflicting_table=TABLE_NAME,conflicting_constraint=CONSTRAINT_NAME;
      -- Only a competing canonical primary-key insertion permits re-probing.
      -- No timer, attempt count or unrelated constraint failure is swallowed.
      IF current_setting('transaction_isolation')<>'read committed' OR NOT EXISTS(
        SELECT FROM pg_constraint c WHERE c.conname=conflicting_constraint AND c.contype='p'
          AND c.conrelid=to_regclass(format('%I.%I',conflicting_schema,conflicting_table))
          AND pg_partition_root(c.conrelid) IN ('public.entity'::regclass,'public.physicality'::regclass)
      ) THEN RAISE; END IF;
      retried:=true;
    END;
  END LOOP;
  IF classified IS NOT NULL THEN EXECUTE format('DROP TABLE %s',classified); END IF;
  EXECUTE format('TRUNCATE %s,%s',e,p);
END $$;
COMMENT ON FUNCTION laplace_merge_stage(regclass,regclass) IS
  'Consume a paired formed batch in Hilbert order. Retain optional Native client presence classifications; verify only claimed-new IDs once for concurrent overlap; insert absent IDs and enrich existing carriers. No global apply lock, destination anti-join or conflict-driven insertion.';
