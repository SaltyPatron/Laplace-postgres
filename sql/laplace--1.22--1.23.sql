\echo Use "ALTER EXTENSION laplace UPDATE TO '1.23'" to load this file. \quit

-- Explicit claim kind defines testimony. Binary and n-ary strands share the
-- same source-rooted solver; logical arity includes every RLE occurrence.
-- Pending observations are cumulative per (claim, source, dependence root).
CREATE OR REPLACE FUNCTION laplace_attest_series(pending regclass, changed blake3[] DEFAULT NULL) RETURNS bigint
LANGUAGE plpgsql VOLATILE PARALLEL UNSAFE AS $$
DECLARE qualified text; claims blake3[]; bad boolean; affected bigint;
BEGIN
  SELECT format('%I.%I', n.nspname, c.relname) INTO qualified
  FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
  WHERE c.oid = pending AND n.nspname = 'stage' AND c.relpersistence = 'u' AND c.relkind = 'r';
  IF qualified IS NULL THEN
    RAISE EXCEPTION 'laplace_attest_series requires an UNLOGGED stage table' USING ERRCODE = '22023';
  END IF;
  IF changed IS NULL THEN
    EXECUTE format('SELECT array_agg(claim ORDER BY claim) FROM (SELECT DISTINCT claim FROM %s) c', qualified) INTO claims;
  ELSE
    SELECT array_agg(c ORDER BY c) INTO claims FROM (SELECT DISTINCT unnest(changed) AS c) selected;
  END IF;
  IF claims IS NULL THEN RETURN 0; END IF;

  -- At most sixteen ordered transaction locks, not a shared-memory lock per
  -- claim. A second writer takes a fresh statement snapshot after acquiring
  -- these locks, so it includes evidence committed by the first writer.
  PERFORM pg_advisory_xact_lock(1279348803, h)
  FROM (SELECT DISTINCT get_byte(decode(left(c::text, 2), 'hex'), 0) / 16 AS h
        FROM unnest(claims) c ORDER BY h) buckets;

  EXECUTE format('WITH p AS MATERIALIZED (SELECT * FROM %s WHERE claim = ANY($1))
    SELECT EXISTS (SELECT FROM p WHERE witness IS NULL OR games IS NULL OR score IS NULL OR trust IS NULL
      OR games <= 0 OR games > 9007199254740992 OR NOT score BETWEEN 0 AND 1 OR NOT trust BETWEEN -1 AND 1)
    OR EXISTS (SELECT FROM p GROUP BY claim, witness, lineage HAVING count(*) <> 1)
    OR EXISTS (SELECT FROM p GROUP BY witness, lineage HAVING count(DISTINCT trust) <> 1)
    OR EXISTS (SELECT FROM unnest($1) c LEFT JOIN public.physicality p ON p.entity = c
      WHERE p.entity IS NULL OR NOT laplace_mask_has_value(p.mask, ''kind'', ''claim'')
         OR laplace_path_length(p.path) < 2)', qualified) INTO bad USING claims;
  IF bad THEN
    RAISE EXCEPTION 'laplace_attest_series: invalid cumulative series, witness metadata or claim path' USING ERRCODE = '22023';
  END IF;

  EXECUTE format('WITH RECURSIVE
    p AS MATERIALIZED (SELECT * FROM %1$s WHERE claim = ANY($1)),
    observed AS MATERIALIZED (
      SELECT a.claim, a.trunk AS witness, coalesce(a.dependence_root, w.lineage) AS lineage, a.games::bigint AS games,
             a.score / a.games AS score, a.trust
      FROM laplace_attested_series($1) a JOIN public.witness w ON w.id = a.trunk
      WHERE a.witness AND a.games > 0
        AND NOT EXISTS (SELECT FROM p WHERE p.claim = a.claim AND p.witness = a.trunk)
      UNION ALL
      SELECT p.claim, p.witness, p.lineage, p.games, p.score, p.trust FROM p
    ),
    parents AS MATERIALIZED (
      SELECT id, lineage FROM public.witness
    ),
    ancestry(initial, root) AS (
      SELECT DISTINCT coalesce(lineage, witness), coalesce(lineage, witness) FROM observed
      UNION
      SELECT a.initial, p.lineage FROM ancestry a JOIN parents p ON p.id = a.root
      WHERE p.lineage IS NOT NULL AND p.lineage <> a.root
    ),
    terminal AS (
      SELECT a.initial, a.root FROM ancestry a LEFT JOIN parents p ON p.id = a.root
      WHERE p.id IS NULL OR p.lineage IS NULL OR p.lineage = a.root
    ),
    grouped AS (
      SELECT o.claim, array_agg(t.root ORDER BY o.witness, o.lineage) AS roots,
             array_agg(o.games ORDER BY o.witness, o.lineage) AS games,
             array_agg(CASE WHEN o.trust < 0 THEN 1 - o.score ELSE o.score END ORDER BY o.witness, o.lineage) AS scores,
             array_agg(abs(o.trust) ORDER BY o.witness, o.lineage) AS trusts
      FROM observed o LEFT JOIN terminal t ON t.initial = coalesce(o.lineage, o.witness) GROUP BY o.claim
    ),
    written AS (INSERT INTO public.consensus AS old (claim, rating, deviation, volatility, matches)
      SELECT g.claim, s.rating, s.deviation, s.volatility, s.matches
      FROM grouped g CROSS JOIN LATERAL laplace_series(g.roots, g.games, g.scores, g.trusts) s
      ORDER BY g.claim
    ON CONFLICT (claim) DO UPDATE SET rating = EXCLUDED.rating, deviation = EXCLUDED.deviation,
      volatility = EXCLUDED.volatility, matches = EXCLUDED.matches RETURNING claim),
    removed AS (DELETE FROM public.consensus old WHERE old.claim = ANY($1)
      AND NOT EXISTS (SELECT FROM grouped g WHERE g.claim = old.claim) RETURNING claim)
    SELECT (SELECT count(*) FROM written) + (SELECT count(*) FROM removed)', qualified) INTO affected USING claims;
  RETURN affected;
END $$;
COMMENT ON FUNCTION laplace_attest_series(regclass, blake3[]) IS
  'Continuously merge cumulative UNLOGGED stage series with containment evidence, fold transitive witness.lineage roots in Native, and atomically replace affected consensus rows. Replays do not add games twice. Negative trust reverses the outcome before the bounded reliability transform.';
