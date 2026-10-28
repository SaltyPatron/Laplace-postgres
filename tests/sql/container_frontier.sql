\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
BEGIN;
-- Real Native IDs in one hash range create a dense bitmap under the caller's
-- small budget. Padding changes heap placement only, never identity or path.
ALTER TABLE public.physicality ADD COLUMN placement_padding text STORAGE PLAIN;
CREATE TEMP TABLE dense_expected AS
WITH parts AS MATERIALIZED (
 SELECT i,ids,laplace_compose(ids) id FROM generate_series(1,2000000) i
 CROSS JOIN LATERAL (SELECT ARRAY[laplace_cp_id(CASE WHEN i%2=0 THEN 65 ELSE 66 END),laplace_cp_id(4096+i/2)] ids) q
 WHERE 4096+i/2 NOT BETWEEN 55296 AND 57343 AND left(laplace_compose(ids)::text,2)='00'
 LIMIT 6000
), composed AS (
 SELECT i,id,laplace_path(ids) path,
  ST_GeomFromEWKB((SELECT laplace_centroid4d_ewkb(ST_GeomFromEWKB(laplace_cp_coord_ewkb(laplace_codepoint(x)))) FROM unnest(ids) x)) coord
 FROM parts
)
SELECT i,id,coord,laplace_hilbert4(coord) # '-9223372036854775808'::bigint AS hilbert,
 ST_SetPoint(path,0,ST_MakePoint(ST_X(ST_PointN(path,1)),ST_Y(ST_PointN(path,1)),ST_Z(ST_PointN(path,1)),ST_M(ST_PointN(path,1))+1073741824)) path
FROM composed;
INSERT INTO public.entity(id,tier,coord,hilbert) SELECT id,1,coord,hilbert FROM dense_expected;
INSERT INTO public.physicality(entity,tier,hilbert,path,placement_padding)
 SELECT id,1,hilbert,path,repeat('x',2000) FROM dense_expected ORDER BY i;
ANALYZE public.physicality;
SET LOCAL work_mem='64kB';
DO $$ DECLARE found bigint; expected bigint; leaf regclass; BEGIN
 SELECT count(*) INTO expected FROM dense_expected WHERE i%2=0;
 SELECT tableoid::regclass INTO leaf FROM public.physicality LIMIT 1;
 IF pg_relation_size(leaf)/current_setting('block_size')::int<1500 THEN
  RAISE EXCEPTION 'fixture is not dense enough to exercise lossy bitmap rechecks';
 END IF;
 SELECT count(*) INTO found FROM public.laplace_attested(ARRAY[public.laplace_cp_id(65)]) r
 JOIN dense_expected e ON e.id=r.trunk WHERE e.i%2=0 AND r.games=1 AND r.tokens=1;
 IF expected<2000 OR found<>expected OR (SELECT count(*) FROM public.laplace_attested(ARRAY[public.laplace_cp_id(65)]))<>expected THEN
  RAISE EXCEPTION 'dense GIN frontier lost true matches or admitted lossy false positives: %',found;
 END IF;
END $$;
ROLLBACK;
SELECT 'containers: dense bitmap, exact rechecks, virtual floor and canonical range routing passed';

BEGIN;
-- Global selection count is not per-leaf density. A few valid compositions
-- can index many distinct constituents. Sparse misses must retain indexed
-- access even when the input set exceeds the number of stored paths.
CREATE TEMP TABLE sparse_expected AS
WITH children AS (
 SELECT i,array_agg(laplace_cp_id(20000+i*100+j) ORDER BY j) ids
 FROM generate_series(0,119) i CROSS JOIN generate_series(0,99) j GROUP BY i
), paths AS (
 SELECT laplace_compose(ids) id,laplace_path(ids) path,
 ST_GeomFromEWKB((SELECT laplace_centroid4d_ewkb(ST_GeomFromEWKB(laplace_cp_coord_ewkb(laplace_codepoint(x)))) FROM unnest(ids) x)) coord
 FROM children
)
SELECT *,laplace_hilbert4(coord) # '-9223372036854775808'::bigint AS hilbert FROM paths;
INSERT INTO public.entity(id,tier,coord,hilbert) SELECT id,1,coord,hilbert FROM sparse_expected;
INSERT INTO public.physicality(entity,tier,hilbert,path) SELECT id,1,hilbert,path FROM sparse_expected;
ANALYZE public.physicality;
DO $$ DECLARE before_scans bigint; after_scans bigint; found bigint; selected oid[]; BEGIN
 SELECT array_agg(h.inhrelid) INTO selected FROM pg_inherits h
 WHERE h.inhparent='public.physicality'::regclass AND pg_relation_size(h.inhrelid)>0;
 SELECT coalesce(sum(seq_scan),0) INTO before_scans FROM pg_stat_xact_all_tables WHERE relid=ANY(selected);
 SELECT count(*) INTO found FROM public.laplace_attested(ARRAY(
   SELECT public.laplace_cp_id(i) FROM generate_series(100000,100499) i));
 SELECT coalesce(sum(seq_scan),0) INTO after_scans FROM pg_stat_xact_all_tables WHERE relid=ANY(selected);
 IF found<>0 OR after_scans<>before_scans THEN
  RAISE EXCEPTION 'sparse global selection scanned physicality heaps: rows %, scans %',found,after_scans-before_scans;
 END IF;
END $$;
ROLLBACK;
SELECT 'containers: sparse global selections retain GIN access above leaf row counts';
