\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
-- Pure decomposition: no table is read or written.
DO $$ DECLARE n bigint; BEGIN
  -- Separators always separate at word tier, in source order, each segment
  -- the identity of its own surface.
  SELECT count(*) INTO n FROM laplace_segments('3.14 a_b') s
   WHERE (s.i,s.segment) IN ((1,'3'),(2,'.'),(3,'14'),(4,' '),(5,'a'),(6,'_'),(7,'b'))
     AND s.id=laplace_text_id(s.segment);
  IF n<>7 OR (SELECT count(*) FROM laplace_segments('3.14 a_b'))<>7 THEN
    RAISE EXCEPTION 'separators did not separate, or a segment is not the identity of its surface';
  END IF;
  -- A single sentence's segments are its trunk's parts; the segments of any
  -- text recompose it byte for byte, across sentences and repeats.
  IF ARRAY(SELECT id FROM laplace_segments('hello world') ORDER BY i)<>laplace_parts('hello world')
     OR laplace_compose(ARRAY(SELECT id FROM laplace_segments('hello world') ORDER BY i))<>laplace_id('hello world') THEN
    RAISE EXCEPTION 'a sentence''s segments are not its trunk''s constituents';
  END IF;
  IF (SELECT string_agg(segment,'' ORDER BY i) FROM laplace_segments(E'The cat sat.  It ran!\nbanana banana'))
       <>E'The cat sat.  It ran!\nbanana banana'
     OR (SELECT count(*) FROM laplace_segments(E'The cat sat.  It ran!\nbanana banana'))
       <=cardinality(laplace_parts(E'The cat sat.  It ran!\nbanana banana')) THEN
    RAISE EXCEPTION 'segments do not recompose the text, or stopped above word tier';
  END IF;
  -- Nothing to segment.
  IF EXISTS(SELECT FROM laplace_segments('')) THEN
    RAISE EXCEPTION 'empty text has segments';
  END IF;
END $$;
SELECT 'segments: word-tier segments in source order, separators split, identities and byte-exact recomposition passed';
