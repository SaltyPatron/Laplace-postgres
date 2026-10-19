\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;

DO $$
DECLARE
  noun bit(64) := laplace_mask_slot('upos', 'NOUN');
  number bit(64) := laplace_mask_slot('upos', 'NUM');
  animal bit(64) := laplace_mask_slot('lexfile', 'noun.animal');
  subject bit(64) := laplace_mask_slot('deprel', 'nsubj');
  agent bit(64) := laplace_mask_slot('vnrole', 'Agent');
  claim bit(64) := laplace_mask_slot('kind', 'claim');
  record bit(64) := laplace_mask_slot('kind', 'record');
  combined bit(64);
BEGIN
  IF bit_length(noun) <> 64 OR octet_length(noun) <> 8 THEN RAISE EXCEPTION 'not a 64-bit carrier'; END IF;
  combined := laplace_mask_merge(laplace_mask_merge(noun, animal), laplace_mask_merge(subject, agent));
  combined := laplace_mask_merge(combined, claim);
  IF NOT laplace_mask_valid(combined)
     OR laplace_mask_value(combined, 'upos') <> 7
     OR laplace_mask_value(combined, 'lexfile') <> 5
     OR NOT laplace_mask_has_value(combined, 'deprel', 'nsubj')
     OR NOT laplace_mask_has_value(combined, 'vnrole', 'Agent')
     OR NOT laplace_mask_has_value(combined, 'kind', 'claim') THEN
    RAISE EXCEPTION 'a bank failed packing or extraction';
  END IF;
  IF laplace_mask_has_value(number, 'upos', 'NOUN')
     OR (number & laplace_mask_field('upos')) = noun THEN
    RAISE EXCEPTION 'NUM must not match the NOUN enum field';
  END IF;
  IF (combined & laplace_mask_field('upos')) <> noun
     OR (combined & claim) <> claim
     OR laplace_mask_merge(noun, animal) <> (noun | animal)
     OR laplace_mask_merge(claim, record) <> (claim | record)
     OR laplace_mask_value(claim | record, 'kind') <> -2 THEN
    RAISE EXCEPTION 'field filtering or compatible OR merge failed';
  END IF;
  IF laplace_mask_slot('upos', 'not_a_part_of_speech') IS NOT NULL
     OR laplace_mask_value(B'0'::bit(64), 'upos') <> -1
     OR NOT laplace_mask_has_all(B'0'::bit(64), '{}'::smallint[])
     OR laplace_mask_has_any(B'0'::bit(64), '{}'::smallint[])
     OR laplace_mask_has(claim, (-1)::smallint)
     OR laplace_mask_has(claim, 64::smallint)
     OR NOT laplace_mask_has(set_bit(B'0'::bit(64), 63, 1), 63::smallint) THEN
    RAISE EXCEPTION 'missing values, empty queries, or bit 63 failed';
  END IF;
  BEGIN
    PERFORM laplace_mask_merge(noun, number);
    RAISE EXCEPTION 'a conflicting enum was silently ORed into a third value';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
  BEGIN
    PERFORM laplace_mask_has(B'1', 0::smallint);
    RAISE EXCEPTION 'a non-64-bit carrier was accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL;
  END;
  BEGIN
    PERFORM laplace_mask_has_all(claim, ARRAY[NULL]::smallint[]);
    RAISE EXCEPTION 'a null bit was accepted';
  EXCEPTION WHEN null_value_not_allowed THEN NULL;
  END;
END $$;

CREATE SCHEMA stage;
CREATE UNLOGGED TABLE stage.mask_entity (LIKE public.entity INCLUDING DEFAULTS INCLUDING CONSTRAINTS);
CREATE UNLOGGED TABLE stage.mask_physicality (LIKE public.physicality INCLUDING DEFAULTS INCLUDING CONSTRAINTS);

INSERT INTO stage.mask_entity (id, tier, coord, hilbert, mask)
VALUES (laplace_id('ab'), laplace_tier('ab'), laplace_coord('ab'), laplace_hilbert('ab'), laplace_mask_slot('upos', 'NOUN'));
INSERT INTO public.entity SELECT * FROM stage.mask_entity
ON CONFLICT (id) DO UPDATE SET mask = laplace_mask_merge(entity.mask, EXCLUDED.mask);
TRUNCATE stage.mask_entity;
INSERT INTO stage.mask_entity (id, tier, coord, hilbert, mask)
VALUES (laplace_id('ab'), 9, laplace_coord('ab'), laplace_hilbert('ab'), laplace_mask_slot('lexfile', 'noun.animal'));
INSERT INTO public.entity SELECT * FROM stage.mask_entity
ON CONFLICT (id) DO UPDATE SET mask = laplace_mask_merge(entity.mask, EXCLUDED.mask);

INSERT INTO stage.mask_physicality (entity, tier, hilbert, path, mask)
VALUES (laplace_id('ab'), laplace_tier('ab'), laplace_hilbert('ab'),
        laplace_path(ARRAY[laplace_cp_id(97), laplace_cp_id(98)]), laplace_mask_slot('kind', 'claim'));
INSERT INTO public.physicality SELECT * FROM stage.mask_physicality
ON CONFLICT (entity) DO UPDATE SET mask = laplace_mask_merge(physicality.mask, EXCLUDED.mask);
TRUNCATE stage.mask_physicality;
INSERT INTO stage.mask_physicality (entity, tier, hilbert, path, mask)
VALUES (laplace_id('ab'), 9, laplace_hilbert('ab'),
        laplace_path(ARRAY[laplace_cp_id(97), laplace_cp_id(98)]),
        laplace_mask_slot('kind', 'record') | laplace_mask_slot('deprel', 'nsubj'));
INSERT INTO public.physicality SELECT * FROM stage.mask_physicality
ON CONFLICT (entity) DO UPDATE SET mask = physicality.mask | EXCLUDED.mask;

SET enable_seqscan = off;
DO $$
DECLARE query_plan json;
BEGIN
  IF (SELECT count(*) FROM public.entity WHERE id = laplace_id('ab')) <> 1
     OR NOT (SELECT laplace_mask_has_value(mask, 'upos', 'NOUN') AND laplace_mask_has_value(mask, 'lexfile', 'noun.animal')
             FROM public.entity WHERE id = laplace_id('ab')) THEN
    RAISE EXCEPTION 'global ID uniqueness or entity mask merge failed';
  END IF;
  IF (SELECT count(*) FROM public.physicality WHERE entity = laplace_id('ab')) <> 1
     OR NOT (SELECT laplace_mask_has_value(mask, 'kind', 'claim') AND laplace_mask_has_value(mask, 'kind', 'record')
                    AND laplace_mask_has_value(mask, 'deprel', 'nsubj')
             FROM public.physicality WHERE entity = laplace_id('ab')) THEN
    RAISE EXCEPTION 'physicality mask OR merge failed';
  END IF;
  IF (SELECT count(*) FROM public.physicality WHERE path @> ARRAY[laplace_cp_id(97)] AND mask ? 0::smallint) <> 1 THEN
    RAISE EXCEPTION 'the real combined path/mask GIN query failed';
  END IF;
  EXECUTE 'EXPLAIN (FORMAT JSON) SELECT entity FROM public.physicality WHERE path @> ARRAY[laplace_cp_id(97)] AND mask ? 0::smallint'
    INTO query_plan;
  IF query_plan::text NOT LIKE '%Bitmap Index Scan%' THEN RAISE EXCEPTION 'the query did not use its GIN index'; END IF;
END $$;
RESET enable_seqscan;

SELECT 'mask: 64-bit packing, enum filtering, set merges, global IDs and GIN passed';
