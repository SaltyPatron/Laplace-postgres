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
  -- Phase-2 lexical & taxonomy fixtures
  w_lex_sense blake3;
  w_syn_rel blake3 := pg_temp.put_text('synonym');
  w_hyponym blake3 := pg_temp.put_text('hyponym');
  w_en blake3 := pg_temp.put_text('en');
  w_lex_word blake3 := pg_temp.put_text(tag || 'canine');
  w_lex_synset blake3 := pg_temp.put_text(tag || 'syn01');
  w_lex_sense_node blake3;
  w_def_direct blake3 := pg_temp.put_text(tag || 'directdef');
  w_def_syn blake3 := pg_temp.put_text(tag || 'synsetdef');
  w_syn_peer blake3 := pg_temp.put_text(tag || 'hound');
  cl_lex_direct blake3;
  cl_lex_bridge blake3;
  cl_lex_syn_def blake3;
  cl_lex_sense_syn blake3;
  n_hop1 integer;
  n_hop2 integer;
  lex_wits bigint;
  lex_max_len integer;
  w_tax_root blake3 := pg_temp.put_text(tag || 'dog');
  w_tax_up1 blake3 := pg_temp.put_text(tag || 'canid');
  w_tax_up2 blake3 := pg_temp.put_text(tag || 'mammal');
  w_tax_dn1 blake3 := pg_temp.put_text(tag || 'terrier');
  w_tax_dn2 blake3 := pg_temp.put_text(tag || 'beagle');
  w_tax_dn_inv blake3 := pg_temp.put_text(tag || 'poodle');
  n_tax_self integer;
  n_tax_up integer;
  n_tax_down integer;
  n_tax_max_d integer;
  n_tax_capped integer;
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

  -- 4. laplace_lexical_resolve: 1-hop direct + 2-hop sense/synset-bridged claims, witnesses, labels & validation
  w_lex_sense := pg_temp.put_node(ARRAY[pg_temp.put_text('lexical'), laplace_cp_id(32), pg_temp.put_text('sense')]);
  IF w_lex_sense IS DISTINCT FROM laplace_text_id('lexical sense') THEN
    RAISE EXCEPTION 'test setup mismatch for lexical sense ID';
  END IF;
  w_lex_sense_node := pg_temp.put_node(ARRAY[w_lex_word, w_en, w_lex_synset]);

  cl_lex_direct := pg_temp.put_node(ARRAY[w_lex_word, w_def, w_def_direct], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl_lex_direct, 2080.0, 40.0, 0.06, 5) ON CONFLICT DO NOTHING;
  INSERT INTO attestation(claim, witness, lineage, games, tokens, score, "position", trust)
    VALUES (cl_lex_direct, w_w1, NULL, 2, 2, 1.0, 1, NULL),
           (cl_lex_direct, w_w2, NULL, 3, 3, 1.0, 1, NULL)
    ON CONFLICT DO NOTHING;

  cl_lex_bridge := pg_temp.put_node(ARRAY[w_lex_word, w_lex_sense, w_lex_sense_node], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl_lex_bridge, 2000.0, 45.0, 0.06, 3) ON CONFLICT DO NOTHING;

  cl_lex_syn_def := pg_temp.put_node(ARRAY[w_lex_synset, w_def, w_def_syn], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl_lex_syn_def, 2040.0, 42.0, 0.06, 4) ON CONFLICT DO NOTHING;

  cl_lex_sense_syn := pg_temp.put_node(ARRAY[w_lex_sense_node, w_syn_rel, w_syn_peer], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl_lex_sense_syn, 1960.0, 50.0, 0.06, 2) ON CONFLICT DO NOTHING;

  SELECT count(*) FILTER (WHERE hop = 1 AND via_claim IS NULL AND claim = blake3_send(cl_lex_direct)
                            AND relation_label = 'definition' AND target_label = tag || 'directdef'),
         count(*) FILTER (WHERE hop = 2 AND via_claim = blake3_send(cl_lex_bridge)
                            AND ((claim = blake3_send(cl_lex_syn_def) AND target_label = tag || 'synsetdef')
                              OR (claim = blake3_send(cl_lex_sense_syn) AND relation_label = 'synonym' AND target_label = tag || 'hound')))
    INTO n_hop1, n_hop2
    FROM public.laplace_lexical_resolve(blake3_send(w_lex_word), ARRAY['definition', 'synonym'], 20, 0);
  IF n_hop1 <> 1 OR n_hop2 <> 2 THEN
    RAISE EXCEPTION 'laplace_lexical_resolve failed 1-hop/2-hop resolution (hop1=%, hop2=%)', n_hop1, n_hop2;
  END IF;

  SELECT witnesses INTO lex_wits
    FROM public.laplace_lexical_resolve(blake3_send(w_lex_word), ARRAY['definition'], 20, 0)
   WHERE claim = blake3_send(cl_lex_direct) LIMIT 1;
  IF lex_wits IS DISTINCT FROM 2 THEN
    RAISE EXCEPTION 'laplace_lexical_resolve expected 2 distinct witnesses, got %', lex_wits;
  END IF;

  SELECT max(octet_length(target_label)) INTO lex_max_len
    FROM public.laplace_lexical_resolve(blake3_send(w_lex_word), ARRAY['definition'], 20, 6);
  IF lex_max_len > 6 THEN
    RAISE EXCEPTION 'laplace_lexical_resolve exceeded max_text=6 (got %)', lex_max_len;
  END IF;

  BEGIN
    PERFORM * FROM public.laplace_lexical_resolve(E'\\x0102'::bytea, ARRAY['definition'], 10, 0);
    RAISE EXCEPTION 'laplace_lexical_resolve failed to reject short term bytea';
  EXCEPTION WHEN invalid_binary_representation THEN NULL; END;

  BEGIN
    PERFORM * FROM public.laplace_lexical_resolve(blake3_send(w_lex_word), ARRAY['definition'], -1, 0);
    RAISE EXCEPTION 'laplace_lexical_resolve failed to reject negative lim';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;

  -- 5. laplace_taxonomy_tree: multi-depth up & down (explicit hyponym + inverse hypernym), caps & validation
  cl := pg_temp.put_node(ARRAY[w_tax_root, w_hyp, w_tax_up1], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl, 2050.0, 40.0, 0.06, 4) ON CONFLICT DO NOTHING;
  cl := pg_temp.put_node(ARRAY[w_tax_up1, w_hyp, w_tax_up2], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl, 2020.0, 45.0, 0.06, 3) ON CONFLICT DO NOTHING;
  cl := pg_temp.put_node(ARRAY[w_tax_root, w_hyponym, w_tax_dn1], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl, 1980.0, 45.0, 0.06, 3) ON CONFLICT DO NOTHING;
  cl := pg_temp.put_node(ARRAY[w_tax_dn1, w_hyponym, w_tax_dn2], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl, 1940.0, 50.0, 0.06, 2) ON CONFLICT DO NOTHING;
  cl := pg_temp.put_node(ARRAY[w_tax_dn_inv, w_hyp, w_tax_root], c_mask);
  INSERT INTO consensus(claim, rating, deviation, volatility, matches)
    VALUES (cl, 1960.0, 48.0, 0.06, 2) ON CONFLICT DO NOTHING;

  SELECT count(*) FILTER (WHERE direction = 'self' AND depth = 0 AND parent IS NULL AND child = blake3_send(w_tax_root)),
         count(*) FILTER (WHERE direction = 'up' AND ((depth = 1 AND parent = blake3_send(w_tax_root) AND child_label = tag || 'canid')
                                                   OR (depth = 2 AND parent = blake3_send(w_tax_up1) AND child_label = tag || 'mammal'))),
         count(*) FILTER (WHERE direction = 'down' AND ((depth = 1 AND child_label = tag || 'terrier')
                                                     OR (depth = 1 AND child_label = tag || 'poodle')
                                                     OR (depth = 2 AND parent = blake3_send(w_tax_dn1) AND child_label = tag || 'beagle')))
    INTO n_tax_self, n_tax_up, n_tax_down
    FROM public.laplace_taxonomy_tree(blake3_send(w_tax_root), ARRAY['hypernym'], ARRAY['hyponym'], 3, 64, 160);
  IF n_tax_self <> 1 OR n_tax_up <> 2 OR n_tax_down <> 3 THEN
    RAISE EXCEPTION 'laplace_taxonomy_tree mismatch (self=%, up=%, down=%)', n_tax_self, n_tax_up, n_tax_down;
  END IF;

  SELECT max(depth), count(*) INTO n_tax_max_d, n_tax_capped
    FROM public.laplace_taxonomy_tree(blake3_send(w_tax_root), ARRAY['hypernym'], ARRAY['hyponym'], 1, 3, 160);
  IF n_tax_max_d > 1 OR n_tax_capped > 3 THEN
    RAISE EXCEPTION 'laplace_taxonomy_tree failed depth/node bounds (max_d=%, count=%)', n_tax_max_d, n_tax_capped;
  END IF;

  BEGIN
    PERFORM * FROM public.laplace_taxonomy_tree(blake3_send(w_tax_root), ARRAY['hypernym'], ARRAY['hyponym'], 7, 64, 160);
    RAISE EXCEPTION 'laplace_taxonomy_tree failed to reject max_depth=7';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;

  BEGIN
    PERFORM * FROM public.laplace_taxonomy_tree(blake3_send(w_tax_root), ARRAY['hypernym'], ARRAY['hyponym'], 2, 0, 160);
    RAISE EXCEPTION 'laplace_taxonomy_tree failed to reject max_nodes=0';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;

ROLLBACK;
SELECT 'read_ops: laplace_response_field, laplace_leaderboard, laplace_browse_entities, laplace_lexical_resolve, and laplace_taxonomy_tree verified';
