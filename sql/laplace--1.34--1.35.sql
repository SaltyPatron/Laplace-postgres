\echo Use "ALTER EXTENSION laplace UPDATE TO '1.35'" to load this file. \quit

CREATE OR REPLACE FUNCTION laplace_series_snapshot(pending regclass, changed blake3[] DEFAULT NULL,
  OUT claim blake3, OUT rating double precision, OUT deviation double precision,
  OUT volatility double precision, OUT matches bigint) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_series_snapshot'
  LANGUAGE C STABLE PARALLEL UNSAFE SET search_path = public, pg_temp;

CREATE FUNCTION laplace_stage_snapshot(pending regclass, changed regclass,
  OUT claim blake3, OUT rating double precision, OUT deviation double precision,
  OUT volatility double precision, OUT matches bigint) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_series_snapshot'
  LANGUAGE C STABLE PARALLEL UNSAFE SET search_path = public, pg_temp;
COMMENT ON FUNCTION laplace_stage_snapshot(regclass, regclass) IS
  'One Native calculation for the supplied stage-resident claim set, sharing indexed carrier proofs, witness registry, lineage resolution and containment DAG; no fixed claim slicing or recursive SQL.';

CREATE FUNCTION laplace_attest_stage(pending regclass, changed regclass) RETURNS bigint
LANGUAGE plpgsql VOLATILE PARALLEL UNSAFE SET search_path = public, pg_temp AS $$
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
  WITH evaluated AS MATERIALIZED (SELECT * FROM laplace_stage_snapshot(pending,changed)),
  updated AS (
    UPDATE public.consensus old SET rating=e.rating,deviation=e.deviation,
      volatility=e.volatility,matches=e.matches FROM evaluated e
    WHERE e.rating IS NOT NULL AND old.claim=e.claim RETURNING old.claim
  ), inserted AS (
    INSERT INTO public.consensus (claim,rating,deviation,volatility,matches)
    SELECT e.claim,e.rating,e.deviation,e.volatility,e.matches FROM evaluated e
    WHERE e.rating IS NOT NULL AND NOT EXISTS(SELECT FROM updated u WHERE u.claim=e.claim)
    ORDER BY e.claim RETURNING claim
  ), removed AS (
    DELETE FROM public.consensus old USING evaluated e WHERE old.claim=e.claim AND e.rating IS NULL RETURNING old.claim
  ) SELECT (SELECT count(*) FROM updated)+(SELECT count(*) FROM inserted)+(SELECT count(*) FROM removed) INTO affected;
  RETURN affected;
END $$;

CREATE FUNCTION laplace_apply_series(pending regclass, delta regclass) RETURNS bigint
LANGUAGE plpgsql VOLATILE PARALLEL UNSAFE SET search_path = public, pg_temp AS $$
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
    WHERE s.staged AND p.claim=s.claim AND p.witness=s.witness AND p.lineage IS NOT DISTINCT FROM s.lineage;
    INSERT INTO %1$s (claim,witness,lineage,games,score,trust)
    SELECT claim,witness,lineage,games,score,trust FROM %2$s WHERE NOT staged',accumulated,incoming);
  affected := laplace_attest_stage(pending,delta);
  EXECUTE format('TRUNCATE %s',incoming);
  RETURN affected;
END $$;

CREATE OR REPLACE FUNCTION laplace_attest_series(pending regclass, changed blake3[] DEFAULT NULL) RETURNS bigint
LANGUAGE plpgsql VOLATILE PARALLEL UNSAFE SET search_path = public, pg_temp AS $$
DECLARE qualified text; claims blake3[]; affected bigint;
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

  WITH evaluated AS MATERIALIZED (SELECT * FROM laplace_series_snapshot(pending,claims)),
  updated AS (
    UPDATE public.consensus old SET rating=e.rating,deviation=e.deviation,
      volatility=e.volatility,matches=e.matches FROM evaluated e
    WHERE e.rating IS NOT NULL AND old.claim=e.claim RETURNING old.claim
  ), inserted AS (
    INSERT INTO public.consensus (claim,rating,deviation,volatility,matches)
    SELECT e.claim,e.rating,e.deviation,e.volatility,e.matches FROM evaluated e
    WHERE e.rating IS NOT NULL AND NOT EXISTS(SELECT FROM updated u WHERE u.claim=e.claim)
    ORDER BY e.claim RETURNING claim
  ), removed AS (
    DELETE FROM public.consensus old USING evaluated e WHERE old.claim=e.claim AND e.rating IS NULL RETURNING old.claim
  ) SELECT (SELECT count(*) FROM updated)+(SELECT count(*) FROM inserted)+(SELECT count(*) FROM removed) INTO affected;
  RETURN affected;
END $$;
COMMENT ON FUNCTION laplace_attest_series(regclass, blake3[]) IS
  'Publish the shared Native series snapshot under ordered consensus bucket locks. Preserve cumulative replay, signed trust, dependence folding and removal of claims without evidence.';

CREATE FUNCTION laplace_present_ids(ids blake3[],OUT id blake3,OUT ordinal bigint) RETURNS SETOF record
 AS 'MODULE_PATHNAME','laplace_present_ids' LANGUAGE C STABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION laplace_present_ids(blake3[]) IS
 'Indexed stored-ID presence for a complete client frontier. Native partition routing and btree array scans preserve input occurrences without per-ID executor/partition restarts; Tier-0 membership belongs to the client cache.';
