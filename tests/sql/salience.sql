\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
BEGIN;

CREATE FUNCTION pg_temp.put_node(parts blake3[], metadata bigint[] DEFAULT NULL, carrier bit(64) DEFAULT B'0'::bit(64)) RETURNS blake3
LANGUAGE plpgsql AS $$
DECLARE
  composed blake3 := laplace_compose(parts);
  path geometry := laplace_path(parts);
  point geometry;
  coord geometry;
  altitude smallint;
  hilbert bigint;
  i integer;
BEGIN
  SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(
           coalesce(e.coord, ST_GeomFromEWKB(laplace_cp_coord_ewkb(laplace_codepoint(p)))))),
         (max(coalesce(e.tier, 0)) + 1)::smallint
    INTO coord, altitude
    FROM unnest(parts) p LEFT JOIN entity e ON e.id = p;
  hilbert := laplace_hilbert4(coord) # '-9223372036854775808'::bigint;
  IF metadata IS NOT NULL THEN
    IF cardinality(metadata) <> ST_NPoints(path) THEN
      RAISE EXCEPTION 'metadata length mismatch';
    END IF;
    FOR i IN 1..cardinality(metadata) LOOP
      point := ST_PointN(path, i);
      path := ST_SetPoint(path, i - 1, ST_MakePoint(ST_X(point), ST_Y(point), ST_Z(point), metadata[i]::double precision));
    END LOOP;
  END IF;
  INSERT INTO entity (id, tier, coord, hilbert, mask)
    VALUES (composed, altitude, coord, hilbert, carrier) ON CONFLICT DO NOTHING;
  INSERT INTO physicality (entity, tier, hilbert, path, mask)
    VALUES (composed, altitude, hilbert, path, carrier) ON CONFLICT DO NOTHING;
  RETURN composed;
END $$;

CREATE FUNCTION pg_temp.make_word(txt text) RETURNS blake3
LANGUAGE plpgsql AS $$
DECLARE
  cps blake3[];
  g1 blake3;
  g2 blake3;
BEGIN
  txt := txt || '_' || pg_current_xact_id()::text;
  SELECT array_agg(laplace_cp_id(ascii(c)) ORDER BY n) INTO cps
    FROM regexp_split_to_table(txt, '') WITH ORDINALITY t(c, n);
  -- Compose via two Tier-1 halves so every word sits canonically at Tier 2.
  g1 := pg_temp.put_node(cps[1:greatest(2, cardinality(cps) / 2)]);
  g2 := pg_temp.put_node(cps[(greatest(2, cardinality(cps) / 2) + 1):cardinality(cps)]);
  RETURN pg_temp.put_node(ARRAY[g1, g2]);
END $$;

DO $$
DECLARE
  sp blake3 := laplace_cp_id(32);
  qm blake3 := laplace_cp_id(63);
  w_how blake3 := pg_temp.make_word('salhowphaseone');
  w_does blake3 := pg_temp.make_word('saldoesphaseone');
  w_work blake3 := pg_temp.make_word('salworkphaseone');
  w_light blake3 := pg_temp.make_word(repeat(chr(9889), 12) || 'sallight' || repeat(chr(9889), 12));
  w_sherlock blake3 := pg_temp.make_word('salsherlockphaseone');
  w_holmes blake3 := pg_temp.make_word('salholmesphaseone');
  w_sh_span blake3;
  w_other_v blake3 := pg_temp.make_word('salotherverbp1');
  w_other_s blake3 := pg_temp.make_word('salothersubjp1');
  w_f blake3;
  w_fn_y blake3 := pg_temp.make_word('salfnyphaseone');
  w_bad blake3 := pg_temp.make_word('salbadtypephaseone');
  w_good blake3 := pg_temp.make_word('salgoodtypephaseone');
  w_tc blake3 := pg_temp.make_word('salwitnesstc');
  w_other blake3 := pg_temp.make_word('salwitnessother');
  w_empty blake3 := pg_temp.make_word('salwitnessempty');
  q_full blake3[];
  q_sh blake3[];
  q_hd blake3[];
  seq_before bigint;
  seq_after bigint;
  r record;
  tot_sal double precision;
  s_how double precision;
  s_does double precision;
  s_work double precision;
  s_light double precision;
  sl_how double precision;
  sl_does double precision;
  sl_light double precision;
  sl_sh double precision;
  sl_hm double precision;
  lc_bad double precision;
  lc_fny double precision;
  lc_good double precision;
  lc_iso double precision;
  claim_id blake3;
  c_mask bit(64) := laplace_mask_slot('kind', 'claim');
  m_loss bigint := (2::bigint << 33) | (2::bigint << 30) | 1;
  t_a blake3;
  t_b blake3;
  d_a blake3;
  d_b blake3;
  target_t3 blake3;
  distract_t3 blake3;
  picked blake3;
  picked_route smallint;
BEGIN
  -- 19 "How does <filler>" sentences (with question mark)
  FOR i IN 1..19 LOOP
    w_f := pg_temp.make_word('salfillhd' || i::text);
    PERFORM pg_temp.put_node(ARRAY[w_how, sp, w_does, sp, w_f, qm]);
  END LOOP;
  -- 1 target question: "How does lightning work?" -> co(how,does)=20
  q_full := ARRAY[w_how, sp, w_does, sp, w_light, sp, w_work, qm];
  PERFORM pg_temp.put_node(q_full);
  -- 1 additional sentence with lightning -> df(lightning)=2
  w_f := pg_temp.make_word('salfilllight2');
  PERFORM pg_temp.put_node(ARRAY[w_light, sp, w_f]);
  -- 40 additional sentences with how (not does) -> df(how)=60
  FOR i IN 1..40 LOOP
    w_f := pg_temp.make_word('salfillh' || i::text);
    PERFORM pg_temp.put_node(ARRAY[w_how, sp, w_other_v, sp, w_f]);
  END LOOP;
  -- 40 additional sentences with does (not how) -> df(does)=60
  FOR i IN 1..40 LOOP
    w_f := pg_temp.make_word('salfilld' || i::text);
    PERFORM pg_temp.put_node(ARRAY[w_other_s, sp, w_does, sp, w_f]);
  END LOOP;
  -- 14 additional sentences with work -> df(work)=15
  FOR i IN 1..14 LOOP
    w_f := pg_temp.make_word('salfillw' || i::text);
    PERFORM pg_temp.put_node(ARRAY[w_work, sp, w_f]);
  END LOOP;

  -- "Sherlock Holmes" collocation (df=3, df=4, co=3) + stored higher-tier concept
  FOR i IN 1..3 LOOP
    w_f := pg_temp.make_word('salfillsh' || i::text);
    PERFORM pg_temp.put_node(ARRAY[w_sherlock, sp, w_holmes, sp, w_f]);
  END LOOP;
  w_f := pg_temp.make_word('salfillhm4');
  PERFORM pg_temp.put_node(ARRAY[w_holmes, sp, w_f]);
  w_sh_span := pg_temp.put_node(ARRAY[w_sherlock, sp, w_holmes]);

  -- Zero seq_scan on physicality%
  SELECT coalesce(sum(seq_scan), 0) INTO seq_before
    FROM pg_stat_xact_all_tables WHERE relname LIKE 'physicality%';

  -- Verify exact df, info_bits, glue=0, sum=1.0, lightning > 4 * how/does
  tot_sal := 0.0;
  FOR r IN SELECT * FROM laplace_salience(q_full) ORDER BY ord LOOP
    tot_sal := tot_sal + r.salience;
    IF abs(r.info_bits - (-ln((r.df + 1.0) / (r.total + 1.0)) / ln(2.0))) > 1e-12 THEN
      RAISE EXCEPTION 'info_bits formula mismatch on ord %: got % (df=%, total=%)', r.ord, r.info_bits, r.df, r.total;
    END IF;
    IF r.id IN (sp, qm) THEN
      IF r.salience <> 0.0 THEN
        RAISE EXCEPTION 'glue token ord % must have salience=0, got %', r.ord, r.salience;
      END IF;
    ELSIF r.id = w_how THEN
      IF r.df <> 60 THEN RAISE EXCEPTION 'expected df(how)=60, got %', r.df; END IF;
      s_how := r.salience; sl_how := r.span_lift;
    ELSIF r.id = w_does THEN
      IF r.df <> 60 THEN RAISE EXCEPTION 'expected df(does)=60, got %', r.df; END IF;
      s_does := r.salience; sl_does := r.span_lift;
    ELSIF r.id = w_work THEN
      IF r.df <> 15 THEN RAISE EXCEPTION 'expected df(work)=15, got %', r.df; END IF;
      s_work := r.salience;
    ELSIF r.id = w_light THEN
      IF r.df <> 2 THEN RAISE EXCEPTION 'expected df(lightning)=2, got %', r.df; END IF;
      s_light := r.salience; sl_light := r.span_lift;
    END IF;
  END LOOP;

  IF abs(tot_sal - 1.0) > 1e-9 THEN
    RAISE EXCEPTION 'salience sum must be 1.0, got %', tot_sal;
  END IF;
  IF s_light <= 4.0 * s_how OR s_light <= 4.0 * s_does THEN
    RAISE EXCEPTION 'anchor salience (%) did not exceed 4x frame salience (how=%, does=%)', s_light, s_how, s_does;
  END IF;
  IF sl_how >= 0.0 OR sl_does >= 0.0 OR sl_light <= 0.0 THEN
    RAISE EXCEPTION 'unexpected full-question span_lift (how=%, does=%, light=%)', sl_how, sl_does, sl_light;
  END IF;

  -- Standalone frame vs specific span
  q_hd := ARRAY[w_how, sp, w_does];
  SELECT span_lift INTO sl_how FROM laplace_salience(q_hd) WHERE id = w_how;
  SELECT span_lift INTO sl_does FROM laplace_salience(q_hd) WHERE id = w_does;
  IF sl_how >= 0.0 OR sl_does >= 0.0 THEN
    RAISE EXCEPTION 'standalone frame "How does" span_lift should be < 0, got how=%, does=%', sl_how, sl_does;
  END IF;

  q_sh := ARRAY[w_sherlock, sp, w_holmes];
  SELECT span_lift INTO sl_sh FROM laplace_salience(q_sh) WHERE id = w_sherlock;
  SELECT span_lift INTO sl_hm FROM laplace_salience(q_sh) WHERE id = w_holmes;
  IF sl_sh <= 0.0 OR sl_hm <= 0.0 THEN
    RAISE EXCEPTION 'specific span "Sherlock Holmes" span_lift should be > 0, got sh=%, hm=%', sl_sh, sl_hm;
  END IF;

  -- Causal loss_contrast: M-outcome LOSS + attestation WIN/LOSS + shared bystander cancellation + scope isolation
  FOR i IN 1..3 LOOP
    w_f := pg_temp.make_word('salloss' || i::text);
    claim_id := pg_temp.put_node(ARRAY[w_fn_y, w_bad, w_f], ARRAY[1, m_loss, 1], c_mask);
    INSERT INTO attestation(claim, witness, lineage, games, tokens, score, "position", trust)
      VALUES (claim_id, w_tc, NULL, 1, 1, 0.0, i, 0.90);
    w_f := pg_temp.make_word('salwin' || i::text);
    claim_id := pg_temp.put_node(ARRAY[w_fn_y, w_good, w_f], NULL, c_mask);
    INSERT INTO attestation(claim, witness, lineage, games, tokens, score, "position", trust)
      VALUES (claim_id, w_tc, NULL, 1, 1, 1.0, i, 0.90);
  END LOOP;
  -- Opposing witness w_other says w_bad won (must NOT affect w_tc scope!)
  w_f := pg_temp.make_word('salotherw');
  claim_id := pg_temp.put_node(ARRAY[w_bad, w_f], NULL, c_mask);
  INSERT INTO attestation(claim, witness, lineage, games, tokens, score, "position", trust)
    VALUES (claim_id, w_other, NULL, 10, 10, 1.0, 1, 0.95);

  SELECT loss_contrast INTO lc_bad FROM laplace_salience(ARRAY[w_fn_y, w_bad, w_good], ARRAY[w_tc]) WHERE id = w_bad;
  SELECT loss_contrast INTO lc_fny FROM laplace_salience(ARRAY[w_fn_y, w_bad, w_good], ARRAY[w_tc]) WHERE id = w_fn_y;
  SELECT loss_contrast INTO lc_good FROM laplace_salience(ARRAY[w_fn_y, w_bad, w_good], ARRAY[w_tc]) WHERE id = w_good;
  IF lc_bad <= 0.5 OR abs(lc_fny) > 1e-9 OR lc_good >= -0.5 THEN
    RAISE EXCEPTION 'causal loss_contrast failed (bad=%, fny=%, good=%)', lc_bad, lc_fny, lc_good;
  END IF;

  SELECT loss_contrast INTO lc_iso FROM laplace_salience(ARRAY[w_fn_y, w_bad], ARRAY[w_empty]) WHERE id = w_bad;
  IF lc_iso <> 0.0 THEN
    RAISE EXCEPTION 'empty witness scope leaked loss_contrast=%', lc_iso;
  END IF;

  -- Salience-weighted centroid in laplace_couple GiST k=1 (using target_t3 placed at salience centroid without changing df)
  t_b := pg_temp.make_word(repeat(chr(9889), 12) || 'asllight' || repeat(chr(9889), 12));
  t_a := pg_temp.put_node(
      array_fill(t_b, ARRAY[greatest(1, round(1000.0 * s_light)::int)]) ||
      array_fill(w_work, ARRAY[greatest(1, round(1000.0 * s_work)::int)]) ||
      array_fill(w_how, ARRAY[greatest(1, round(1000.0 * s_how)::int)]) ||
      array_fill(w_does, ARRAY[greatest(1, round(1000.0 * s_does)::int)]));
  target_t3 := t_a;
  d_a := pg_temp.put_node(ARRAY[sp, sp, qm]);
  d_b := pg_temp.put_node(ARRAY[qm, sp, qm]);
  distract_t3 := pg_temp.put_node(ARRAY[d_a, w_how, d_b, w_does]);

  SELECT c.entity, c.route INTO picked, picked_route
    FROM laplace_couple(q_full, 1::bigint, '{}'::blake3[], 0::smallint, 0.0::double precision, 1) c
   WHERE c.route = 2 LIMIT 1;
  IF picked IS DISTINCT FROM target_t3 THEN
    RAISE EXCEPTION 'laplace_couple did not nominate salience-anchored curve (got % route %, expected % vs distractor %)',
      picked, picked_route, target_t3, distract_t3;
  END IF;

  -- Ultra-high-df hub short-circuit (spaces, punctuation, and "the" on the populated corpus)
  DECLARE
    t0 timestamptz := clock_timestamp();
    elapsed double precision;
  BEGIN
    PERFORM * FROM laplace_salience(ARRAY[sp, qm, laplace_id('the'), w_light]);
    elapsed := extract(epoch FROM (clock_timestamp() - t0));
    IF elapsed >= 0.5 THEN
      RAISE EXCEPTION 'high-df salience exceeded 0.5s budget: %s', elapsed;
    END IF;
  END;

  SELECT coalesce(sum(seq_scan), 0) INTO seq_after
    FROM pg_stat_xact_all_tables WHERE relname LIKE 'physicality%';
  IF seq_after <> seq_before THEN
    RAISE EXCEPTION 'laplace_salience triggered physicality seq_scan (% -> %)', seq_before, seq_after;
  END IF;
END $$;

ROLLBACK;
SELECT 'salience: GIN df above hub cap, info_bits, glue zeroing, span_lift, causal loss_contrast, witness scope, and couple centroid weighting verified with zero seq_scan';
