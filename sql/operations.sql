-- Current operation definitions, shared by cold install and development update.
-- A load owns its private relations through a session lock on the control
-- connection. Catalog comments retain that ownership after a client crash;
-- neither a timer nor a currently idle writer proves a load is abandoned.
CREATE OR REPLACE FUNCTION @extschema@.laplace_stage_own(load_token bigint, objects regclass[]) RETURNS void
    LANGUAGE plpgsql
    AS $$
DECLARE object record; owner_pid integer := pg_backend_pid();
BEGIN
  IF load_token IS NULL OR load_token<=0 OR cardinality(objects)=0 OR objects IS NULL OR EXISTS (
    SELECT FROM unnest(objects) supplied(id)
    LEFT JOIN pg_class c ON c.oid=supplied.id
    LEFT JOIN pg_namespace n ON n.oid=c.relnamespace
    WHERE c.oid IS NULL OR n.nspname<>'stage' OR c.relkind<>'r' OR c.relpersistence<>'u'
      OR NOT pg_has_role(c.relowner,'USAGE')
      OR c.relname !~ ('^lp_[a-z][a-z0-9_]*_' || load_token || '(_[0-9]+)?$')
      OR obj_description(c.oid,'pg_class') IS NOT NULL
  ) THEN
    RAISE EXCEPTION 'laplace_stage_own requires unclaimed private load tables owned by the caller' USING ERRCODE='22023';
  END IF;
  PERFORM pg_advisory_lock(1279348804,owner_pid);
  BEGIN
    FOR object IN SELECT DISTINCT c.oid,c.relname FROM unnest(objects) supplied(id)
      JOIN pg_class c ON c.oid=supplied.id ORDER BY c.oid
    LOOP
      EXECUTE format('COMMENT ON TABLE stage.%I IS %L',object.relname,
        format('laplace:stage-load:1:%s:%s',owner_pid,load_token));
    END LOOP;
  EXCEPTION WHEN OTHERS THEN
    PERFORM pg_advisory_unlock(1279348804,owner_pid);
    RAISE;
  END;
END $$;

CREATE OR REPLACE FUNCTION @extschema@.laplace_stage_retire()
    RETURNS TABLE (load_token bigint, relations bigint, allocated_bytes bigint)
    LANGUAGE plpgsql
    AS $$
DECLARE abandoned record; targets text; owner_pid integer;
BEGIN
  FOR abandoned IN
    SELECT DISTINCT description FROM pg_description d JOIN pg_class c ON c.oid=d.objoid
    JOIN pg_namespace n ON n.oid=c.relnamespace
    WHERE d.classoid='pg_class'::regclass AND d.objsubid=0 AND n.nspname='stage'
      AND c.relkind='r' AND c.relpersistence='u' AND pg_has_role(c.relowner,'USAGE')
      AND d.description ~ '^laplace:stage-load:1:[1-9][0-9]*:[1-9][0-9]*$'
    ORDER BY description
  LOOP
    owner_pid := split_part(abandoned.description,':',4)::integer;
    load_token := split_part(abandoned.description,':',5)::bigint;
    -- PostgreSQL session locks survive transaction boundaries and disappear
    -- on disconnect. Skip our own owner too: advisory locks are reentrant.
    IF owner_pid=pg_backend_pid() OR NOT pg_try_advisory_xact_lock(1279348804,owner_pid) THEN CONTINUE; END IF;
    SELECT string_agg(format('stage.%I',c.relname),',' ORDER BY c.oid),count(*),sum(pg_total_relation_size(c.oid))
      INTO targets,relations,allocated_bytes
    FROM pg_description d JOIN pg_class c ON c.oid=d.objoid JOIN pg_namespace n ON n.oid=c.relnamespace
    WHERE d.classoid='pg_class'::regclass AND d.objsubid=0 AND d.description=abandoned.description
      AND n.nspname='stage' AND c.relkind='r' AND c.relpersistence='u' AND pg_has_role(c.relowner,'USAGE')
      AND c.relname ~ ('^lp_[a-z][a-z0-9_]*_' || load_token || '(_[0-9]+)?$');
    IF targets IS NOT NULL THEN
      -- No CASCADE: an unexpected dependency preserves the whole transaction.
      EXECUTE 'DROP TABLE ' || targets;
      RETURN NEXT;
    END IF;
  END LOOP;
END $$;

CREATE OR REPLACE FUNCTION @extschema@.laplace_apply_series(pending regclass, delta regclass) RETURNS bigint
    LANGUAGE plpgsql

    AS $_$
DECLARE accumulated text; incoming text; affected bigint;
BEGIN
  SELECT format('%I.%I',n.nspname,c.relname) INTO accumulated
  FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
  WHERE c.oid=pending AND n.nspname='stage' AND c.relpersistence='u' AND c.relkind='r';
  SELECT format('%I.%I',n.nspname,c.relname) INTO incoming
  FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
  WHERE c.oid=delta AND n.nspname='stage' AND c.relpersistence='u' AND c.relkind='r';
  IF accumulated IS NULL OR incoming IS NULL OR pending=delta THEN
    RAISE EXCEPTION 'laplace_apply_series requires distinct UNLOGGED stage tables' USING ERRCODE='22023';
  END IF;
  -- The Native accumulator already knows which cumulative series it staged.
  -- Keep that proof: update those rows and insert only genuinely new series.
  EXECUTE format('UPDATE %1$s p SET games=s.games,score=s.score,trust=s.trust FROM %2$s s
    WHERE s.staged AND p.claim OPERATOR(@extschema@.=) s.claim AND p.witness OPERATOR(@extschema@.=) s.witness AND (p.lineage OPERATOR(@extschema@.=) s.lineage OR (p.lineage IS NULL AND s.lineage IS NULL));
    INSERT INTO %1$s (claim,witness,lineage,games,score,trust)
    SELECT claim,witness,lineage,games,score,trust FROM %2$s WHERE NOT staged',accumulated,incoming);
  affected := @extschema@.laplace_attest_stage(pending,delta);
  EXECUTE format('TRUNCATE %s',incoming);
  RETURN affected;
END $_$;

CREATE OR REPLACE FUNCTION @extschema@.laplace_attest_series(pending regclass, changed @extschema@.blake3[] DEFAULT NULL::@extschema@.blake3[]) RETURNS bigint
    LANGUAGE plpgsql

    AS $$
DECLARE qualified text; claims @extschema@.blake3[]; affected bigint;
BEGIN
  SELECT format('%I.%I', n.nspname, c.relname) INTO qualified
  FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
  WHERE c.oid = pending AND n.nspname = 'stage' AND c.relpersistence = 'u' AND c.relkind = 'r';
  IF qualified IS NULL THEN
    RAISE EXCEPTION 'laplace_attest_series requires an UNLOGGED stage table' USING ERRCODE = '22023';
  END IF;
  IF changed IS NULL THEN
    EXECUTE format('SELECT array_agg(c.claim ORDER BY c.claim) FROM (SELECT DISTINCT claim FROM %s) c', qualified) INTO claims;
  ELSE
    SELECT array_agg(c ORDER BY c) INTO claims FROM (SELECT DISTINCT unnest(changed) AS c) selected;
  END IF;
  IF claims IS NULL THEN RETURN 0; END IF;

  PERFORM pg_advisory_xact_lock(1279348803, h)
  FROM (SELECT DISTINCT get_byte(decode(left(c::text, 2), 'hex'), 0) / 16 AS h
        FROM unnest(claims) c ORDER BY h) buckets;

  WITH evaluated AS MATERIALIZED (SELECT * FROM @extschema@.laplace_series_snapshot(pending,claims)),
  updated AS (
    UPDATE @extschema@.consensus old SET rating=e.rating,deviation=e.deviation,
      volatility=e.volatility,matches=e.matches FROM evaluated e
    WHERE e.rating IS NOT NULL AND old.claim OPERATOR(@extschema@.=) e.claim RETURNING old.claim
  ), inserted AS (
    INSERT INTO @extschema@.consensus (claim,rating,deviation,volatility,matches)
    SELECT e.claim,e.rating,e.deviation,e.volatility,e.matches FROM evaluated e
    WHERE e.rating IS NOT NULL AND NOT EXISTS(SELECT FROM updated u WHERE u.claim OPERATOR(@extschema@.=) e.claim)
    ORDER BY e.claim RETURNING claim
  ), removed AS (
    DELETE FROM @extschema@.consensus old USING evaluated e WHERE old.claim OPERATOR(@extschema@.=) e.claim AND e.rating IS NULL RETURNING old.claim
  ) SELECT (SELECT count(*) FROM updated)+(SELECT count(*) FROM inserted)+(SELECT count(*) FROM removed) INTO affected;
  RETURN affected;
END $$;

CREATE OR REPLACE FUNCTION @extschema@.laplace_attest_stage(pending regclass, changed regclass) RETURNS bigint
    LANGUAGE plpgsql

    AS $$
DECLARE selected text; affected bigint;
BEGIN
  SELECT format('%I.%I',n.nspname,c.relname) INTO selected
  FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
  WHERE c.oid=changed AND c.relkind='r' AND ((n.nspname='stage' AND c.relpersistence='u')
    OR (n.oid=pg_my_temp_schema() AND c.relpersistence='t'));
  IF selected IS NULL THEN
    RAISE EXCEPTION 'laplace_attest_stage requires an UNLOGGED changed-claim table' USING ERRCODE='22023';
  END IF;
  EXECUTE format('SELECT pg_advisory_xact_lock(1279348803,h) FROM
    (SELECT DISTINCT get_byte(decode(left(claim::text,2),''hex''),0)/16 AS h FROM %s ORDER BY h) buckets',selected);
  WITH evaluated AS MATERIALIZED (SELECT * FROM @extschema@.laplace_stage_snapshot(pending,changed)),
  updated AS (
    UPDATE @extschema@.consensus old SET rating=e.rating,deviation=e.deviation,
      volatility=e.volatility,matches=e.matches FROM evaluated e
    WHERE e.rating IS NOT NULL AND old.claim OPERATOR(@extschema@.=) e.claim RETURNING old.claim
  ), inserted AS (
    INSERT INTO @extschema@.consensus (claim,rating,deviation,volatility,matches)
    SELECT e.claim,e.rating,e.deviation,e.volatility,e.matches FROM evaluated e
    WHERE e.rating IS NOT NULL AND NOT EXISTS(SELECT FROM updated u WHERE u.claim OPERATOR(@extschema@.=) e.claim)
    ORDER BY e.claim RETURNING claim
  ), removed AS (
    DELETE FROM @extschema@.consensus old USING evaluated e WHERE old.claim OPERATOR(@extschema@.=) e.claim AND e.rating IS NULL RETURNING old.claim
  ) SELECT (SELECT count(*) FROM updated)+(SELECT count(*) FROM inserted)+(SELECT count(*) FROM removed) INTO affected;
  RETURN affected;
END $$;

CREATE OR REPLACE FUNCTION @extschema@.laplace_clean_pending_list(index_id regclass) RETURNS bigint
    LANGUAGE plpgsql STRICT SECURITY DEFINER
    SET search_path TO 'pg_catalog', 'pg_temp'
    AS $_$
DECLARE table_id oid; parent_id oid; caller name;
BEGIN
  SELECT i.indrelid INTO table_id
    FROM pg_index i
    JOIN pg_class idx ON idx.oid=i.indexrelid
    JOIN pg_am am ON am.oid=idx.relam
    JOIN pg_class tab ON tab.oid=i.indrelid
    JOIN pg_namespace ns ON ns.oid=tab.relnamespace
    JOIN pg_depend dep ON dep.classid='pg_class'::regclass
      AND dep.objid=tab.oid AND dep.deptype='e'
    JOIN pg_extension ext ON ext.oid=dep.refobjid
    WHERE i.indexrelid=index_id AND am.amname='gin'
      AND ns.oid=(SELECT extnamespace FROM pg_catalog.pg_extension WHERE extname='laplace') AND tab.relname ~ '^physicality_[0-9a-f]{2}$'
      AND tab.relispartition AND ext.extname='laplace';
  IF NOT FOUND THEN
    RAISE EXCEPTION 'index is not an extension-owned Laplace physicality GIN index'
      USING ERRCODE='insufficient_privilege';
  END IF;
  parent_id := pg_partition_root(table_id);
  caller := nullif(current_setting('role'),'none');
  IF caller IS NULL THEN caller := session_user; END IF;
  IF NOT (has_table_privilege(caller,table_id,'INSERT') OR has_table_privilege(caller,parent_id,'INSERT'))
     OR NOT (has_table_privilege(caller,table_id,'UPDATE') OR has_table_privilege(caller,parent_id,'UPDATE')) THEN
    RAISE EXCEPTION 'Laplace ingestion write privileges are required'
      USING ERRCODE='insufficient_privilege';
  END IF;
  RETURN pg_catalog.gin_clean_pending_list(index_id);
END $_$;

CREATE OR REPLACE FUNCTION @extschema@.laplace_ensure_stage() RETURNS void
    LANGUAGE plpgsql
    AS $$
BEGIN
  IF to_regclass('stage.entity') IS NOT NULL
     AND to_regclass('stage.physicality') IS NOT NULL
     AND to_regclass('stage.attestation') IS NOT NULL THEN RETURN; END IF;
  PERFORM pg_advisory_xact_lock(1279348803, -1);
  CREATE SCHEMA IF NOT EXISTS stage;
  CREATE UNLOGGED TABLE IF NOT EXISTS stage.entity (LIKE @extschema@.entity INCLUDING DEFAULTS);
  CREATE UNLOGGED TABLE IF NOT EXISTS stage.physicality (LIKE @extschema@.physicality INCLUDING DEFAULTS);
  CREATE UNLOGGED TABLE IF NOT EXISTS stage.attestation (
    claim @extschema@.blake3 NOT NULL, witness @extschema@.blake3 NOT NULL, lineage @extschema@.blake3,
    games bigint NOT NULL CHECK (games > 0),
    score double precision NOT NULL CHECK (score BETWEEN 0 AND 1),
    trust double precision NOT NULL CHECK (trust BETWEEN -1 AND 1)
  );
END $$;

CREATE OR REPLACE FUNCTION @extschema@.laplace_merge_stage(entities regclass, paths regclass, OUT entity_rows bigint, OUT physicality_rows bigint, OUT updated_rows bigint) RETURNS record
    LANGUAGE plpgsql

    AS $_$
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
      OR a.tier NOT BETWEEN 1 AND 255 OR NOT @extschema@.laplace_mask_valid(a.mask)
      OR (NOT %3$s AND (a.coord IS NULL OR a.hilbert IS NULL)))
    OR EXISTS(SELECT FROM %2$s b WHERE b.entity IS NULL OR b.tier IS NULL OR b.mask IS NULL
      OR b.tier NOT BETWEEN 1 AND 255 OR NOT @extschema@.laplace_mask_valid(b.mask)
      OR (NOT %4$s AND (b.path IS NULL OR b.hilbert IS NULL)))
    OR EXISTS(SELECT FROM %1$s a FULL JOIN %2$s b ON b.entity OPERATOR(@extschema@.=) a.id WHERE a.id IS NULL OR b.entity IS NULL)',e,p,entity_proof,path_proof) INTO invalid;
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
    EXECUTE format('CREATE UNLOGGED TABLE %s (id @extschema@.blake3 PRIMARY KEY,entity_present boolean NOT NULL,path_present boolean NOT NULL)',classified);
    EXECUTE format('INSERT INTO %s SELECT a.id,%s,%s FROM %s a JOIN %s b ON b.entity OPERATOR(@extschema@.=) a.id',classified,entity_proof,path_proof,e,p);
    e_class:=classified; p_class:=classified; e_key:='id'; p_key:='id'; e_flag:='entity_present'; p_flag:='path_present';
    e_join:=format(' JOIN %s c ON c.id OPERATOR(@extschema@.=) s.id',classified);
    p_join:=format(' JOIN %s c ON c.id OPERATOR(@extschema@.=) s.entity',classified);
    e_write:='c.entity_present'; p_write:='c.path_present';
  END IF;

  LOOP
    BEGIN
      -- One flat overlap verification of claimed-new IDs, using the same
      -- Native btree membership kernel as tier descent. Positive client
      -- classifications remain the write plan and are not probed again.
      EXECUTE format('UPDATE %1$s c SET %3$I=true
        FROM @extschema@.laplace_present_stage(%1$L::regclass,''@extschema@.entity'',%2$L,%3$L) landed WHERE landed.id OPERATOR(@extschema@.=) c.%2$I',e_class,e_key,e_flag);
      EXECUTE format('UPDATE %1$s c SET %3$I=true
        FROM @extschema@.laplace_present_stage(%1$L::regclass,''@extschema@.physicality'',%2$L,%3$L) landed WHERE landed.id OPERATOR(@extschema@.=) c.%2$I',p_class,p_key,p_flag);
      EXECUTE format('SELECT (SELECT count(*) FROM %1$s WHERE %3$I),(SELECT count(*) FROM %2$s WHERE %4$I),
        (SELECT count(*) FROM %1$s WHERE %3$I IS NOT TRUE)+(SELECT count(*) FROM %2$s WHERE %4$I IS NOT TRUE)',e_class,p_class,e_flag,p_flag)
        INTO known_entities,known_paths,candidates;
      IF retried AND candidates>=previous_candidates THEN
        RAISE EXCEPTION 'concurrent publication requires a fresh transaction snapshot' USING ERRCODE='40001';
      END IF;
      previous_candidates:=candidates;
      EXECUTE format('INSERT INTO @extschema@.entity(id,tier,coord,hilbert,mask)
        SELECT s.id,s.tier,s.coord,s.hilbert,s.mask FROM %s s%s
        WHERE NOT %s ORDER BY s.hilbert,s.id',e,e_join,e_write);
      GET DIAGNOSTICS entity_rows=ROW_COUNT;
      updated_rows:=0;
      IF known_entities>0 THEN
        updated_rows:=@extschema@.laplace_enrich_stage(entities,'@extschema@.entity',e_class::regclass,known_entities);
      END IF;
      EXECUTE format('INSERT INTO @extschema@.physicality(entity,tier,hilbert,path,mask)
        SELECT s.entity,s.tier,s.hilbert,s.path,s.mask FROM %s s%s
        WHERE NOT %s ORDER BY s.hilbert,s.entity',p,p_join,p_write);
      GET DIAGNOSTICS physicality_rows=ROW_COUNT;
      changed:=0;
      IF known_paths>0 THEN
        changed:=@extschema@.laplace_enrich_stage(paths,'@extschema@.physicality',p_class::regclass,known_paths);
      END IF;
      updated_rows:=updated_rows+changed;
      IF physicality_rows>0 OR changed>0 THEN PERFORM @extschema@.laplace_clean_stage(paths); END IF;
      EXIT;
    EXCEPTION WHEN unique_violation THEN
      GET STACKED DIAGNOSTICS conflicting_schema=SCHEMA_NAME,conflicting_table=TABLE_NAME,conflicting_constraint=CONSTRAINT_NAME;
      -- Only a competing canonical primary-key insertion permits re-probing.
      -- No timer, attempt count or unrelated constraint failure is swallowed.
      IF current_setting('transaction_isolation')<>'read committed' OR NOT EXISTS(
        SELECT FROM pg_constraint c WHERE c.conname=conflicting_constraint AND c.contype='p'
          AND c.conrelid=to_regclass(format('%I.%I',conflicting_schema,conflicting_table))
          AND pg_partition_root(c.conrelid) IN ('@extschema@.entity'::regclass,'@extschema@.physicality'::regclass)
      ) THEN RAISE; END IF;
      retried:=true;
    END;
  END LOOP;
  IF classified IS NOT NULL THEN EXECUTE format('DROP TABLE %s',classified); END IF;
  EXECUTE format('TRUNCATE %s,%s',e,p);
END $_$;

CREATE OR REPLACE FUNCTION @extschema@.laplace_recoordinate(apply boolean DEFAULT false, OUT entity_rows bigint, OUT physicality_rows bigint) RETURNS record
    LANGUAGE plpgsql
    AS $$
DECLARE level smallint;
BEGIN
  PERFORM pg_advisory_xact_lock(1279348803, -2);
  IF EXISTS (SELECT FROM @extschema@.entity e FULL JOIN @extschema@.physicality p ON p.entity OPERATOR(@extschema@.=) e.id
      WHERE e.id IS NULL OR p.entity IS NULL OR e.tier NOT BETWEEN 0 AND 255) THEN
    RAISE EXCEPTION 'laplace_recoordinate: unpaired entity/path or invalid tier' USING ERRCODE='23514';
  END IF;
  IF EXISTS (SELECT FROM @extschema@.entity e JOIN @extschema@.physicality p ON p.entity OPERATOR(@extschema@.=) e.id
      WHERE e.tier=0 AND (@extschema@.laplace_codepoint(e.id) IS NULL OR @extschema:postgis@.GeometryType(p.path)<>'POINT'
        OR @extschema@.laplace_path_length(p.path)<>1 OR @extschema@.laplace_vertex_ids(p.path)<>ARRAY[e.id])) THEN
    RAISE EXCEPTION 'laplace_recoordinate: tier0 row is not a canonical scalar self-path' USING ERRCODE='23514';
  END IF;
  IF EXISTS (SELECT FROM @extschema@.entity e JOIN @extschema@.physicality p ON p.entity OPERATOR(@extschema@.=) e.id
      CROSS JOIN LATERAL unnest(@extschema@.laplace_vertex_ids(p.path)) AS vertex(id)
      JOIN @extschema@.entity child ON child.id OPERATOR(@extschema@.=) vertex.id
      WHERE child.tier >= e.tier AND NOT (e.tier=0 AND vertex.id OPERATOR(@extschema@.=) e.id)) THEN
    RAISE EXCEPTION 'laplace_recoordinate: path violates strict child-to-parent tier ordering' USING ERRCODE='23514';
  END IF;
  CREATE TEMP TABLE laplace_coordinate_projection (
    id @extschema@.blake3 PRIMARY KEY, coord @extschema:postgis@.geometry(POINTZM), hilbert bigint NOT NULL
  ) ON COMMIT DROP;
  FOR level IN SELECT DISTINCT tier FROM @extschema@.entity ORDER BY tier LOOP
    WITH inputs AS (
      SELECT p.entity,
        array_agg(vertex.id ORDER BY vertex.id) AS ids,
        array_agg(coalesce(fixed.coord,child.coord) ORDER BY vertex.id) AS coords
      FROM @extschema@.entity e JOIN @extschema@.physicality p ON p.entity OPERATOR(@extschema@.=) e.id
      CROSS JOIN LATERAL unnest(@extschema@.laplace_vertex_ids(p.path)) AS vertex(id)
      LEFT JOIN @extschema@.entity child ON child.id OPERATOR(@extschema@.=) vertex.id
      LEFT JOIN pg_temp.laplace_coordinate_projection fixed ON fixed.id OPERATOR(@extschema@.=) vertex.id
      WHERE e.tier=level GROUP BY p.entity
    )
    INSERT INTO pg_temp.laplace_coordinate_projection
    SELECT i.entity, @extschema:postgis@.ST_GeomFromEWKB(projected.coord_ewkb), projected.hilbert
    FROM inputs i JOIN @extschema@.physicality p ON p.entity OPERATOR(@extschema@.=) i.entity
    JOIN @extschema@.entity e ON e.id OPERATOR(@extschema@.=) i.entity
    CROSS JOIN LATERAL @extschema@.laplace_project_path(i.entity,p.path,i.ids,i.coords) projected
    WHERE @extschema:postgis@.ST_AsEWKB(e.coord) <> projected.coord_ewkb OR e.hilbert <> projected.hilbert
       OR p.hilbert <> projected.hilbert;
  END LOOP;
  SELECT count(*) INTO entity_rows FROM pg_temp.laplace_coordinate_projection f
    JOIN @extschema@.entity e ON e.id OPERATOR(@extschema@.=) f.id WHERE @extschema:postgis@.ST_AsEWKB(e.coord)<>@extschema:postgis@.ST_AsEWKB(f.coord) OR e.hilbert<>f.hilbert;
  SELECT count(*) INTO physicality_rows FROM pg_temp.laplace_coordinate_projection f
    JOIN @extschema@.physicality p ON p.entity OPERATOR(@extschema@.=) f.id WHERE p.hilbert<>f.hilbert;
  IF apply THEN
    UPDATE @extschema@.entity e SET coord=f.coord,hilbert=f.hilbert FROM pg_temp.laplace_coordinate_projection f
      WHERE e.id OPERATOR(@extschema@.=) f.id AND (@extschema:postgis@.ST_AsEWKB(e.coord)<>@extschema:postgis@.ST_AsEWKB(f.coord) OR e.hilbert<>f.hilbert);
    UPDATE @extschema@.physicality p SET hilbert=f.hilbert FROM pg_temp.laplace_coordinate_projection f
      WHERE p.entity OPERATOR(@extschema@.=) f.id AND p.hilbert<>f.hilbert;
  END IF;
  DROP TABLE pg_temp.laplace_coordinate_projection;
END $$;

CREATE OR REPLACE FUNCTION @extschema@.laplace_schema_indexes() RETURNS void
    LANGUAGE plpgsql
    AS $$
BEGIN
  CREATE INDEX IF NOT EXISTS entity_hilbert ON @extschema@.entity (hilbert);
  CREATE INDEX IF NOT EXISTS entity_coord ON @extschema@.entity USING gist (coord @extschema@.laplace_point4d_ops);
  -- 32 MB a partition: a search scans the pending list, so it spills around a batch instead of holding a source.
  -- 4 MB spilled every few thousand paths (full-page images). 256 MB held the list until the source ended.
  CREATE INDEX IF NOT EXISTS physicality_paths ON @extschema@.physicality USING gin (path @extschema@.laplace_path_ops, mask @extschema@.laplace_mask_ops) WITH (gin_pending_list_limit = 32768);
END $$;

CREATE OR REPLACE FUNCTION @extschema@.laplace_restore_function_bindings() RETURNS void
AS 'MODULE_PATHNAME','laplace_restore_function_bindings' LANGUAGE C VOLATILE PARALLEL UNSAFE;

-- Identity-space reference preparation shared by every API adapter.
CREATE OR REPLACE FUNCTION @extschema@.laplace_resolve_refs(ids bytea[],labels text[],OUT ordinal bigint,OUT id bytea,OUT label text,OUT present boolean)
RETURNS SETOF record AS 'MODULE_PATHNAME','laplace_resolve_refs' LANGUAGE C STABLE PARALLEL SAFE;
CREATE OR REPLACE FUNCTION @extschema@.laplace_resolve_ref(reference text) RETURNS bytea
AS 'MODULE_PATHNAME','laplace_resolve_ref' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION @extschema@.laplace_claim_preview(ids @extschema@.blake3[],OUT i bigint,OUT entity @extschema@.blake3,
 OUT predicate @extschema@.blake3,OUT object @extschema@.blake3,OUT type_label text,OUT fact text)
RETURNS SETOF record AS 'MODULE_PATHNAME','laplace_claim_preview' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- Ordered structure is projected from the canonical registered DAG at every tier.
CREATE OR REPLACE FUNCTION @extschema@.laplace_separator_ids() RETURNS @extschema@.blake3[]
AS 'MODULE_PATHNAME','laplace_separator_ids' LANGUAGE C IMMUTABLE PARALLEL SAFE;
CREATE OR REPLACE FUNCTION @extschema@.laplace_ordered_pair_counts(vocab @extschema@.blake3[],lo integer,hi integer,
 roots @extschema@.blake3[] DEFAULT NULL,separators @extschema@.blake3[] DEFAULT @extschema@.laplace_separator_ids(),
 OUT container @extschema@.blake3,OUT gap integer,OUT subject @extschema@.blake3,OUT object @extschema@.blake3,OUT occurrences bigint)
RETURNS SETOF record AS 'MODULE_PATHNAME','laplace_ordered_pair_counts' LANGUAGE C STABLE PARALLEL RESTRICTED;
