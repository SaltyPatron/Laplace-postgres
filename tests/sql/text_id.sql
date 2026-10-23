\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
CREATE TEMP TABLE engine_text_ids (content text, expected blake3);
-- Captured from the locally built Engine CLI using the canonical scalar cache.
INSERT INTO engine_text_ids VALUES
('abandon.01', '2d25f2d444594c0115ab1bdab872167b'),
('i46360', '5bc6df9ccf51dd8f9399b33e113392ec'),
('United States', 'ebe26b3b2d2fcd45455cdf0d61468709'),
('leave-51.2', 'a07f2a034896c2d5a60779c4db1c5c6f'),
('New York', '7400a4cd15dfbd2acd2ceebfbae264c9');
DO $$
DECLARE r record; content text;
BEGIN
  FOR r IN SELECT * FROM engine_text_ids LOOP
    IF laplace_text_id(r.content) <> r.expected OR laplace_id(r.content) <> r.expected THEN
      RAISE EXCEPTION 'SQL text identity differs from Engine for %', r.content;
    END IF;
  END LOOP;
  -- The phrase's coordinate weights its actual three word segments equally,
  -- as Native composition does; a flat codepoint centroid gives another point.
  IF laplace_text_coord_ewkb('United States') IS DISTINCT FROM (
      SELECT laplace_centroid4d_ewkb(point) FROM unnest(ARRAY[
          laplace_text_coord('United'), laplace_text_coord(' '),
          laplace_text_coord('States')]) AS segment(point)) THEN
    RAISE EXCEPTION 'text coordinate flattened the canonical Native tree';
  END IF;
  IF laplace_text_coord_ewkb('') IS NOT NULL OR laplace_text_coord_ewkb(NULL) IS NOT NULL THEN
    RAISE EXCEPTION 'empty-content coordinate semantics failed';
  END IF;
  IF laplace_text_id(U&'A\030A') <> laplace_compose(ARRAY[laplace_cp_id(65),laplace_cp_id(778)])
     OR laplace_text_id(U&'A\030A') = laplace_text_id(U&'\00C5')
     OR laplace_text_id(U&'\+01F44D\+01F3FD') <> laplace_compose(ARRAY[laplace_cp_id(128077),laplace_cp_id(127997)])
     OR laplace_text_id('') IS NOT NULL OR laplace_text_id(NULL) IS NOT NULL THEN
    RAISE EXCEPTION 'grapheme composition, byte preservation or empty-content semantics failed';
  END IF;
  FOREACH content IN ARRAY ARRAY['banana','mississippi',E'First sentence.\nSecond sentence.',E'a\n\nb','  punctuation; and spaces!  '] LOOP
    IF laplace_text_id(content) <> laplace_id(content) THEN
      RAISE EXCEPTION 'Native composition routes disagree for %', content;
    END IF;
  END LOOP;
END $$;
-- A second transaction reuses the backend context and its Native/ICU arenas.
SELECT count(*) = 5 AND bool_and(laplace_text_id(content) = expected) AS repeated_backend_calls FROM engine_text_ids;
SELECT 'text_id: Engine identifiers, phrases, graphemes, RLE and byte preservation passed';
