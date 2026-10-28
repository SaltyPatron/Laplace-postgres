\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
BEGIN;
CREATE FUNCTION pg_temp.pair_coord(a geometry,b geometry) RETURNS geometry
LANGUAGE SQL IMMUTABLE BEGIN ATOMIC
 SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(point)) FROM (VALUES(a),(b)) p(point);
END;
-- A genuine nested composition: every identity, coordinate and path is made
-- from the preceding node and the same floor atom. Altitude is not flattened.
CREATE TEMP TABLE deep_expected AS
WITH RECURSIVE chain(depth,id,coord,path) AS (
 SELECT 0,laplace_cp_id(65),ST_GeomFromEWKB(laplace_cp_coord_ewkb(65)),NULL::geometry
 UNION ALL
 SELECT depth+1,laplace_compose(ARRAY[id,laplace_cp_id(66)]),
  pg_temp.pair_coord(coord,ST_GeomFromEWKB(laplace_cp_coord_ewkb(66))),
  laplace_path(ARRAY[id,laplace_cp_id(66)]) FROM chain WHERE depth<4096
)
SELECT *,laplace_hilbert4(coord) # '-9223372036854775808'::bigint hilbert FROM chain WHERE depth>0;
INSERT INTO entity(id,tier,coord,hilbert) SELECT id,depth::smallint,coord,hilbert FROM deep_expected;
INSERT INTO physicality(entity,tier,hilbert,path) SELECT id,depth::smallint,hilbert,path FROM deep_expected;
SET LOCAL max_stack_depth='100kB';
DO $$ DECLARE root blake3; expected text:='A'||repeat('B',4096); BEGIN
 SELECT id INTO root FROM deep_expected WHERE depth=4096;
 IF laplace_text(root)<>expected OR
    (SELECT count(*) FROM laplace_text_batch(ARRAY[root,laplace_cp_id(65),root])
     WHERE content=CASE WHEN i=2 THEN 'A' ELSE expected END)<>3 OR
    (SELECT content FROM laplace_labels(ARRAY[root]))<>expected THEN
  RAISE EXCEPTION 'deep Native realization lost text, repeated roots or scalar content';
 END IF;
 -- Reject corruption explicitly; neither a depth cutoff nor an endless walk
 -- is an acceptable response to a cycle in a stored carrier.
 UPDATE physicality SET path=laplace_path(ARRAY[root,laplace_cp_id(66)]) WHERE entity=root;
 BEGIN
  PERFORM laplace_text(root);
  RAISE EXCEPTION 'cyclic exact text accepted';
 EXCEPTION WHEN data_exception THEN NULL; END;
 BEGIN
  PERFORM content FROM laplace_labels(ARRAY[root]);
  RAISE EXCEPTION 'cyclic display label accepted';
 EXCEPTION WHEN data_exception THEN NULL; END;
END $$;
ROLLBACK;
SELECT 'realization: 4096 nested tiers, bounded C stack, repeated roots and cycle rejection passed';
