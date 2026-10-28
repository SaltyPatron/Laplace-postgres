\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
BEGIN;
-- Exercise the complete Native traversal across the real partition layout,
-- rather than timing only one geometry predicate. Every stored word is
-- canonical Native content; the missing IDs are lookup keys, not entities.
CREATE TEMP TABLE frontier_words AS
SELECT 'frontier' || chr(97 + i % 26) || chr(97 + (i / 26) % 26) || chr(97 + (i / 676) % 26) AS content
FROM generate_series(0,16999) i;
INSERT INTO public.entity(id,tier,coord,hilbert)
SELECT laplace_id(content),laplace_tier(content),laplace_coord(content),laplace_hilbert(content)
FROM frontier_words;
INSERT INTO public.physicality(entity,tier,hilbert,path)
SELECT laplace_id(content),laplace_tier(content),laplace_hilbert(content),
       laplace_path(ARRAY(SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table(content,'') c))
FROM frontier_words;
ANALYZE public.physicality;
SET LOCAL work_mem='16MB';
DO $$
DECLARE found bigint;
BEGIN
  SELECT count(*) INTO found FROM laplace_attribution(
    ARRAY(SELECT md5(i::text)::blake3 FROM generate_series(100001,220000) i));
  IF found <> 0 THEN RAISE EXCEPTION 'missing frontier acquired witness evidence'; END IF;
END $$;
-- A dense frontier must also retain genuine matches across multiple transfer
-- windows. Annotate the existing canonical first scalar as an assertion.
UPDATE public.physicality SET path=ST_SetPoint(path,0,ST_MakePoint(
 ST_X(ST_PointN(path,1)),ST_Y(ST_PointN(path,1)),ST_Z(ST_PointN(path,1)),
 ST_M(ST_PointN(path,1))+1073741824));
SET LOCAL work_mem='64kB';
DO $$ DECLARE found bigint; BEGIN
 SELECT count(*) INTO found FROM laplace_attested(
  ARRAY[laplace_cp_id(102)] || ARRAY(SELECT md5(i::text)::blake3 FROM generate_series(100001,105000) i));
 IF found<>17000 THEN RAISE EXCEPTION 'dense frontier lost true matches or repeated a transfer window: %',found; END IF;
END $$;
ROLLBACK;
SELECT 'frontier runtime: complete traversal over 17000 canonical paths passed';
