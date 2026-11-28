\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
BEGIN;

CREATE FUNCTION pg_temp.put_node(parts blake3[], carrier bigint DEFAULT 0) RETURNS blake3
LANGUAGE plpgsql AS $$
DECLARE
  composed blake3 := laplace_compose(parts);
  path geometry := laplace_path(parts);
  coord geometry;
  altitude smallint;
  hilbert bigint;
BEGIN
  SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(
           coalesce(e.coord, ST_GeomFromEWKB(laplace_cp_coord_ewkb(laplace_codepoint(p)))))),
         (max(coalesce(e.tier, 0)) + 1)::smallint
    INTO coord, altitude
    FROM unnest(parts) p LEFT JOIN entity e ON e.id = p;
  hilbert := laplace_hilbert4(coord);
  INSERT INTO entity (id, tier, coord, hilbert, mask)
    VALUES (composed, altitude, coord, hilbert, carrier) ON CONFLICT DO NOTHING;
  INSERT INTO physicality (entity, tier, path, mask)
    VALUES (composed, altitude, path, carrier) ON CONFLICT DO NOTHING;
  RETURN composed;
END $$;

CREATE FUNCTION pg_temp.put_text(txt text, carrier bigint DEFAULT 0) RETURNS blake3
LANGUAGE plpgsql AS $$
DECLARE
  cps blake3[];
BEGIN
  SELECT array_agg(laplace_cp_id(ascii(c)) ORDER BY n) INTO cps
    FROM regexp_split_to_table(txt, '') WITH ORDINALITY t(c, n);
  RETURN pg_temp.put_node(cps, carrier);
END $$;

DO $$
DECLARE
  tag text := 'ro' || pg_current_xact_id()::text;
  c_mask bigint := laplace_mask_slot('kind', 'claim');
  w_a blake3 := pg_temp.put_text(tag || 'quasar');
  w_b blake3 := pg_temp.put_text(tag || 'pancake');
  w_b_cap blake3 := pg_temp.put_text(initcap(tag || 'pancake'));
  w_bridge blake3 := pg_temp.put_text(tag || 'shared');
  w_r2_a blake3 := pg_temp.put_text(tag || 'r2targeta');
  w_r2_b blake3 := pg_temp.put_text(tag || 'r2targetb');
  w_def blake3 := pg_temp.put_text('definition');
  w_hyp blake3 := pg_temp.put_text('hypernym');
  w_w1 blake3 := pg_temp.put_text(tag || 'wit1');
  w_w2 blake3 := pg_temp.put_text(tag || 'wit2');
  w_mid blake3;
  cl blake3;
  cl_def blake3;
  cl_hyp blake3;
  n_occ1_r2 integer;
  n_occ2_r2 integer;
  n_bridged integer;
  n_outbound integer;
  wits_found bigint;
  n_band1 integer;
  n_band2 integer;
  n_exact integer;
  n_var integer;
  n_mask integer;
  max_lbl_len integer;
BEGIN
  -- Seed w_a with 20 Round-1 claims (crowding test) + 1 Round-2 hop from w_bridge
  FOR i IN 1..20 LOOP
    w_mid := pg_temp.put_text(tag || 'filla' || i::text);
    cl := pg_temp.put_node(ARRAY[w_a, w_hyp, w_mid], c_mask);
    INSERT INTO consensus(claim, rating, deviation, volatility, matches)
      VALUES (cl, 1800.0 + i, 50.0, 0.06, 2) ON CONFLICT DO NOTHING;
  END LOOP;
  -- Shared bridge node reached from both w_a and w_b, plus R2 hops from each side
  cl_hyp := pg_temp.put_node(ARRAY[w_a, w_hyp, w_bridge], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl_hyp, 2100.0, 40.0, 0.06, 5) ON CONFLICT DO NOTHING;
  INSERT INTO attestation(claim, witness, lineage, games, tokens, score, "position", trust)
    VALUES (w_w1, w_w1, NULL, 0, 0, 0.0, NULL, 0.90),
           (w_w2, w_w2, NULL, 0, 0, 0.0, NULL, 0.90),
           (cl_hyp, w_w1, NULL, 2, 2, 1.0, 1, NULL),
           (cl_hyp, w_w2, NULL, 3, 3, 1.0, 1, NULL)
    ON CONFLICT DO NOTHING;

  cl_def := pg_temp.put_node(ARRAY[w_b, w_def, w_bridge], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl_def, 2050.0, 45.0, 0.06, 4) ON CONFLICT DO NOTHING;

  cl := pg_temp.put_node(ARRAY[w_bridge, w_hyp, w_r2_a], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl, 1950.0, 50.0, 0.06, 3) ON CONFLICT DO NOTHING;

  w_mid := pg_temp.put_text(tag || 'bnode');
  cl := pg_temp.put_node(ARRAY[w_b, w_hyp, w_mid], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl, 1900.0, 50.0, 0.06, 3) ON CONFLICT DO NOTHING;
  cl := pg_temp.put_node(ARRAY[w_mid, w_def, w_r2_b], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl, 1880.0, 50.0, 0.06, 3) ON CONFLICT DO NOTHING;

  -- 1. laplace_response_field: balanced R2, honest claim ID, witnesses, outbound, targets, max_text
  SELECT count(*) FILTER (WHERE round = 2 AND occ = 1),
         count(*) FILTER (WHERE round = 2 AND occ = 2),
         count(*) FILTER (WHERE cardinality(targets) > 0),
         count(*) FILTER (WHERE route = 0 AND outbound IS TRUE),
         max(octet_length(target_label))
    INTO n_occ1_r2, n_occ2_r2, n_bridged, n_outbound, max_lbl_len
    FROM public.laplace_response_field(ARRAY[blake3_send(w_a), blake3_send(w_b)], 8, 2, 0, 10);

  IF n_occ1_r2 < 1 OR n_occ2_r2 < 1 THEN
    RAISE EXCEPTION 'laplace_response_field did not balance R2 per occurrence (occ1=%, occ2=%)', n_occ1_r2, n_occ2_r2;
  END IF;
  IF n_bridged < 1 OR n_outbound < 1 THEN
    RAISE EXCEPTION 'laplace_response_field missing bridges or outbound flag (bridged=%, outbound=%)', n_bridged, n_outbound;
  END IF;
  IF max_lbl_len > 10 THEN
    RAISE EXCEPTION 'laplace_response_field exceeded max_text=10 (got %)', max_lbl_len;
  END IF;

  SELECT witnesses INTO wits_found
    FROM public.laplace_response_field(ARRAY[blake3_send(w_a)], 8, 1, 0, 160)
   WHERE claim = blake3_send(cl_hyp) LIMIT 1;
  IF wits_found IS DISTINCT FROM 2 THEN
    RAISE EXCEPTION 'laplace_response_field expected 2 distinct witnesses for cl_hyp, got %', wits_found;
  END IF;

  -- 2. laplace_leaderboard: governed bands 1 & 2 honored honestly + global mode + invalid band fails loud
  SELECT count(*) FILTER (WHERE band = 1 AND band_name = 'definition'),
         count(*) FILTER (WHERE band = 2 AND band_name = 'taxonomy')
    INTO n_band1, n_band2
    FROM public.laplace_leaderboard(ARRAY[1, 2], 5, 0, 1, 160);
  IF n_band1 < 1 OR n_band2 < 1 THEN
    RAISE EXCEPTION 'laplace_leaderboard missing band 1/2 rows (b1=%, b2=%)', n_band1, n_band2;
  END IF;

  BEGIN
    PERFORM * FROM public.laplace_leaderboard(ARRAY[99], 5, 0, 1, 160);
    RAISE EXCEPTION 'laplace_leaderboard failed to reject unknown band 99';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;

  -- 3. laplace_browse_entities: prefix exact + case_variant, entity_mask GIN, and invalid args fail loud
  SELECT count(*) FILTER (WHERE id = blake3_send(w_b) AND match_kind = 'exact'),
         count(*) FILTER (WHERE id = blake3_send(w_b_cap) AND match_kind = 'case_variant')
    INTO n_exact, n_var
    FROM public.laplace_browse_entities(NULL, NULL, tag || 'pancake', 25, 0, 160);
  IF n_exact <> 1 OR n_var <> 1 THEN
    RAISE EXCEPTION 'laplace_browse_entities prefix lookup failed (exact=%, var=%)', n_exact, n_var;
  END IF;

  SELECT count(*) INTO n_mask
    FROM public.laplace_browse_entities(NULL, ARRAY[laplace_bank_bit('kind', 'claim')::int4], NULL, 25, 0, 160)
   WHERE (mask & c_mask) <> 0;
  IF n_mask < 1 THEN
    RAISE EXCEPTION 'laplace_browse_entities mask_bits GIN scan returned 0 claim rows';
  END IF;

  BEGIN
    PERFORM * FROM public.laplace_browse_entities(NULL, ARRAY[99], NULL, 10, 0, 160);
    RAISE EXCEPTION 'laplace_browse_entities failed to reject out-of-range mask bit 99';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;

ROLLBACK;
SELECT 'read_ops: laplace_response_field, laplace_leaderboard, and laplace_browse_entities verified';
