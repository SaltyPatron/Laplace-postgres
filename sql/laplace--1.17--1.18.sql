\echo Use "ALTER EXTENSION laplace UPDATE TO '1.18'" to load this file. \quit

-- Games remain exact beyond the old signed 32-bit counter. The read operators
-- expose the same bigint, rather than silently truncating an SPI Datum.
ALTER TABLE consensus ALTER COLUMN matches TYPE bigint;
DROP FUNCTION laplace_claims(blake3[], bigint, smallint[], blake3[]);
DROP FUNCTION laplace_claims_each(blake3[], bigint, smallint[], blake3[]);
CREATE FUNCTION laplace_claims(parts blake3[], fan bigint, bits smallint[], refuse blake3[],
  OUT entity blake3, OUT path geometry, OUT rating double precision, OUT deviation double precision,
  OUT volatility double precision, OUT matches bigint) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_claims' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 64;
CREATE FUNCTION laplace_claims_each(ids blake3[], fan bigint, bits smallint[], refuse blake3[],
  OUT i bigint, OUT entity blake3, OUT path geometry, OUT rating double precision, OUT deviation double precision,
  OUT volatility double precision, OUT matches bigint) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_claims_each' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 1024;

CREATE FUNCTION laplace_path_length(geometry) RETURNS bigint
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_series(roots blake3[], games bigint[], scores double precision[], trusts double precision[],
  OUT rating double precision, OUT deviation double precision, OUT volatility double precision,
  OUT matches bigint, OUT witnesses bigint) RETURNS record
  AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION laplace_series(blake3[], bigint[], double precision[], double precision[]) IS
  'Native rooted Glicko-2 posterior: games and mean outcomes, bounded trust, shared source uncertainty. Complete evidence is recomputed from the stock prior; input permutations and replay are identical.';

-- The caller COPYs cumulative per-(claim,witness) series into its UNLOGGED
-- stage partition. Required columns: claim blake3, witness blake3, lineage
-- blake3 (nullable), games bigint, score float8 (MEAN), trust float8. Each pair
-- occurs once. Pending metadata permits continuous updates before the source
-- trunk is atomically registered in public.witness. No public attestation table
-- is introduced: recorded evidence is read from containment, and pending rows
-- replace, rather than add to, the recorded series of the same source.
--
-- changed restricts work to the current COPY window. Other pending rows for
-- those claims remain part of the complete evidence set. The caller retains
-- cumulative rows until its source trunk and containment are committed, then
-- removes that source's pending rows in the same transaction.
CREATE FUNCTION laplace_attest_series(pending regclass, changed blake3[] DEFAULT NULL) RETURNS bigint
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
    OR EXISTS (SELECT FROM p GROUP BY claim, witness HAVING count(*) <> 1)
    OR EXISTS (SELECT FROM p GROUP BY witness HAVING count(DISTINCT (lineage, trust)) <> 1)
    OR EXISTS (SELECT FROM unnest($1) c LEFT JOIN public.physicality p ON p.entity = c
      WHERE p.entity IS NULL OR NOT laplace_mask_has_value(p.mask, ''kind'', ''claim'')
         OR laplace_path_length(p.path) <> 3)', qualified) INTO bad USING claims;
  IF bad THEN
    RAISE EXCEPTION 'laplace_attest_series: invalid cumulative series, witness metadata or claim path' USING ERRCODE = '22023';
  END IF;

  EXECUTE format('WITH RECURSIVE
    p AS MATERIALIZED (SELECT * FROM %1$s WHERE claim = ANY($1)),
    observed AS MATERIALIZED (
      SELECT a.claim, a.trunk AS witness, w.lineage, a.games::bigint AS games,
             a.score / a.games AS score, w.trust
      FROM laplace_attested($1) a JOIN public.witness w ON w.id = a.trunk
      WHERE a.witness AND a.games > 0
        AND NOT EXISTS (SELECT FROM p WHERE p.claim = a.claim AND p.witness = a.trunk)
      UNION ALL
      SELECT p.claim, p.witness, CASE WHEN w.id IS NULL THEN p.lineage ELSE w.lineage END,
             p.games, p.score, CASE WHEN w.id IS NULL THEN p.trust ELSE w.trust END
      FROM p LEFT JOIN public.witness w ON w.id = p.witness
    ),
    parents AS MATERIALIZED (
      SELECT id, lineage FROM public.witness
      UNION SELECT witness, lineage FROM observed o WHERE NOT EXISTS (SELECT FROM public.witness w WHERE w.id = o.witness)
    ),
    ancestry(witness, root) AS (
      SELECT DISTINCT witness, coalesce(lineage, witness) FROM observed
      UNION
      SELECT a.witness, p.lineage FROM ancestry a JOIN parents p ON p.id = a.root
      WHERE p.lineage IS NOT NULL AND p.lineage <> a.root
    ),
    terminal AS (
      SELECT a.witness, a.root FROM ancestry a LEFT JOIN parents p ON p.id = a.root
      WHERE p.id IS NULL OR p.lineage IS NULL OR p.lineage = a.root
    ),
    grouped AS (
      SELECT o.claim, array_agg(t.root ORDER BY o.witness) AS roots,
             array_agg(o.games ORDER BY o.witness) AS games,
             array_agg(CASE WHEN o.trust < 0 THEN 1 - o.score ELSE o.score END ORDER BY o.witness) AS scores,
             array_agg(abs(o.trust) ORDER BY o.witness) AS trusts
      FROM observed o LEFT JOIN terminal t ON t.witness = o.witness GROUP BY o.claim
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
