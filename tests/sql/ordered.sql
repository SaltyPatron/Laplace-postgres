\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
BEGIN;
CREATE FUNCTION pg_temp.ordered_node(parts blake3[]) RETURNS blake3 LANGUAGE plpgsql AS $$
DECLARE id blake3:=laplace_compose(parts); point geometry; altitude smallint; h bigint;
BEGIN
 SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(coalesce(e.coord,
          ST_GeomFromEWKB(laplace_cp_coord_ewkb(laplace_codepoint(p)))))),
        (max(coalesce(e.tier,0))+1)::smallint INTO point,altitude
 FROM unnest(parts) p LEFT JOIN entity e ON e.id=p;
 h:=laplace_hilbert4(point) # '-9223372036854775808'::bigint;
 INSERT INTO entity(id,tier,coord,hilbert) VALUES(id,altitude,point,h) ON CONFLICT DO NOTHING;
 INSERT INTO physicality(entity,tier,hilbert,path) VALUES(id,altitude,h,laplace_path(parts)) ON CONFLICT DO NOTHING;
 RETURN id;
END $$;
DO $$ DECLARE
 a blake3:=laplace_cp_id(65); b blake3:=laplace_cp_id(66); c blake3:=laplace_cp_id(67);
 separator blake3;
 root blake3;
 other blake3;
 orphan blake3;
 counts bigint[];
BEGIN
 IF laplace_parts(laplace_path(ARRAY[b,a,a,c,b])) IS DISTINCT FROM ARRAY[b,a,a,c,b]
    OR laplace_parts(laplace_path(ARRAY[a])) IS DISTINCT FROM ARRAY[a]
    OR laplace_parts(ST_GeomFromText('LINESTRING ZM EMPTY')) IS DISTINCT FROM '{}'::blake3[]
    OR laplace_parts(NULL::geometry) IS NOT NULL THEN
   RAISE EXCEPTION 'acquired path lost ordered constituents or compressed repetitions';
 END IF;
 separator:=pg_temp.ordered_node(ARRAY[laplace_cp_id(13),laplace_cp_id(10)]);
 IF cardinality(laplace_separator_ids())<>85
    OR NOT separator=ANY(laplace_separator_ids())
    OR NOT laplace_cp_id(32)=ANY(laplace_separator_ids()) THEN
   RAISE EXCEPTION 'separator IDs differ from the Native Unicode floor';
 END IF;
 root:=pg_temp.ordered_node(ARRAY[a,a,separator,b,a,c]);
 other:=pg_temp.ordered_node(ARRAY[root,root]);
 orphan:=pg_temp.ordered_node(ARRAY[a,b,a,b]);
 IF EXISTS(SELECT FROM laplace_ordered_pair_counts(ARRAY[a,b,a],1,2,ARRAY[root])) THEN
   RAISE EXCEPTION 'unregistered root admitted';
 END IF;
 INSERT INTO witness(id,trust) VALUES(root,1),(other,1);
 SELECT array_agg(n ORDER BY gap,subject,object) INTO counts FROM (
   SELECT gap,subject,object,sum(occurrences)::bigint n
   FROM laplace_ordered_pair_counts(ARRAY[a,b,a],1,2,ARRAY[root,other,root])
   GROUP BY gap,subject,object
 ) q;
 IF cardinality(counts)<>7 OR (SELECT sum(v) FROM unnest(counts) v)<>7 THEN
   RAISE EXCEPTION 'ordered runs, shared roots, or excluded-object denominator changed: %',counts;
 END IF;
 IF (SELECT sum(occurrences) FROM laplace_ordered_pair_counts(ARRAY[a],1,1,ARRAY[root])
     WHERE subject=a AND object=a)<>1 THEN
   RAISE EXCEPTION 'repeated adjacent child lost';
 END IF;
 IF (SELECT sum(occurrences) FROM laplace_ordered_pair_counts(ARRAY[a],1,1,ARRAY[root])
     WHERE subject=a AND object=b)<>1 THEN
   RAISE EXCEPTION 'separator was not removed before measuring offsets';
 END IF;
 IF NOT EXISTS(SELECT FROM laplace_ordered_pair_counts(ARRAY[a],1,1,ARRAY[root],NULL)
     WHERE subject=a AND object=separator) THEN
   RAISE EXCEPTION 'NULL separator selection did not retain raw structure';
 END IF;
 IF EXISTS(SELECT FROM laplace_ordered_pair_counts('{}',1,2))
    OR EXISTS(SELECT FROM laplace_ordered_pair_counts(NULL,1,2,'{}')) THEN
   RAISE EXCEPTION 'empty selection returned structure';
 END IF;
 IF (SELECT sum(occurrences) FROM laplace_ordered_pair_counts(NULL,1,1,ARRAY[other],NULL)
     WHERE container=other)<>1 THEN
   RAISE EXCEPTION 'higher-tier structure omitted';
 END IF;
 BEGIN
   PERFORM count(*) FROM laplace_ordered_pair_counts(NULL,0,2);
   RAISE EXCEPTION 'invalid offset accepted';
 EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
ROLLBACK;
SELECT 'ordered: compressed runs, registered scope, shared tiers, Unicode separators and selections passed';
