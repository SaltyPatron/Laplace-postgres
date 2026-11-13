\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
-- Walks over the stored DAG and the rated claims: antecedents,
-- upward and downward closure, best-first pull and declared chains. The
-- database is shared: every ID is a leaf no other content holds, and every
-- statement is rolled back.
BEGIN;
CREATE FUNCTION pg_temp.leaf(n integer) RETURNS blake3 LANGUAGE sql IMMUTABLE
RETURN md5('Laplace check regression walks leaf '||n)::blake3;
CREATE FUNCTION pg_temp.place(ids blake3[], t smallint) RETURNS void LANGUAGE sql AS $$
 INSERT INTO entity(id,tier,coord,hilbert)
 SELECT u.id,t,laplace_coord('walks '||u.id::text),laplace_hilbert('walks '||u.id::text) FROM unnest(ids) u(id)
$$;
CREATE FUNCTION pg_temp.hold(id blake3, parts blake3[], t smallint, claim boolean DEFAULT false) RETURNS void LANGUAGE sql AS $$
 SELECT pg_temp.place(ARRAY[id],t);
 INSERT INTO physicality(entity,tier,hilbert,path,mask)
 SELECT id,t,e.hilbert,laplace_path(parts),CASE WHEN claim THEN laplace_mask_slot('kind','claim') ELSE B'0'::bit(64) END
 FROM entity e WHERE e.id=hold.id
$$;

-- Antecedents mirror continuations, runs counted.
DO $$
DECLARE a blake3:=pg_temp.leaf(1); b blake3:=pg_temp.leaf(2); c blake3:=pg_temp.leaf(3); p geometry;
BEGIN
 p:=laplace_path(ARRAY[a,b,b,c]);
 IF laplace_precedes(p,ARRAY[b]) IS DISTINCT FROM ARRAY[a,b]
 OR laplace_follows(p,ARRAY[b]) IS DISTINCT FROM ARRAY[b,c]
 OR laplace_precedes(p,ARRAY[b,c]) IS DISTINCT FROM ARRAY[b]
 OR laplace_precedes(p,ARRAY[a]) IS NOT NULL
 OR laplace_follows(p,ARRAY[c]) IS NOT NULL THEN
  RAISE EXCEPTION 'Native antecedents do not mirror continuations';
 END IF;
END $$;

-- Upward and downward closure: each entity once, by hop and depth.
DO $$
DECLARE a blake3:=pg_temp.leaf(20); b blake3:=pg_temp.leaf(21); x blake3:=pg_temp.leaf(22);
        p1 blake3:=pg_temp.leaf(23); p2 blake3:=pg_temp.leaf(24); p3 blake3:=pg_temp.leaf(25);
BEGIN
 PERFORM pg_temp.place(ARRAY[a,b,x],0::smallint);
 PERFORM pg_temp.hold(p1,ARRAY[a,b],1::smallint);
 PERFORM pg_temp.hold(p2,ARRAY[p1,x],2::smallint);
 PERFORM pg_temp.hold(p3,ARRAY[p2,p1],3::smallint);
 IF (SELECT array_agg(entity::text||':'||hops||':'||tier ORDER BY hops,entity) FROM laplace_above(ARRAY[a],3))
    <>(SELECT array_agg(v.e::text||':'||v.h||':'||v.t ORDER BY v.h,v.e) FROM (VALUES (p1,1,1),(p2,2,2),(p3,2,3)) v(e,h,t))
 OR (SELECT array_agg(entity ORDER BY hops,entity) FROM laplace_above(ARRAY[a],3))
    <>(SELECT array_agg(entity ORDER BY ordinality) FROM laplace_above(ARRAY[a],3) WITH ORDINALITY)
 OR (SELECT array_agg(entity ORDER BY entity) FROM laplace_above(ARRAY[a,p1]))
    IS DISTINCT FROM (SELECT array_agg(v ORDER BY v) FROM unnest(ARRAY[p2,p3]) v)
 OR (SELECT count(*) FROM laplace_above(ARRAY[a],3,1))<>1
 OR EXISTS(SELECT FROM laplace_above(ARRAY[a],0))
 OR EXISTS(SELECT FROM laplace_above(ARRAY[p3],2)) THEN
  RAISE EXCEPTION 'Native upward closure lost a hop, a tier, the cap or the input exclusion';
 END IF;
 IF (SELECT count(*) FROM laplace_below(ARRAY[p3],3))<>6
 OR (SELECT count(*) FROM laplace_below(ARRAY[p3],3) WHERE parent=p1)<>2
 OR (SELECT min(depth) FROM laplace_below(ARRAY[p3],3) WHERE parent=p1)<>2
 OR (SELECT count(*) FROM laplace_below(ARRAY[p3],3) WHERE depth=2)<>4
 OR (SELECT array_agg(child ORDER BY ordinal) FROM laplace_below(ARRAY[p3]) WHERE parent=p3)<>ARRAY[p2,p1]
 OR (SELECT count(*) FROM laplace_below(ARRAY[p3,p3],1))<>2
 OR EXISTS(SELECT FROM laplace_below(ARRAY[p3],0)) THEN
  RAISE EXCEPTION 'Native downward closure expanded a parent twice or lost a depth';
 END IF;
 BEGIN PERFORM laplace_above(ARRAY[a],-1); RAISE EXCEPTION 'negative hops accepted';
 EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;

-- Best-first pull and declared chains over rated claims.
CREATE TEMP TABLE walk_claims(label text, id blake3, parts blake3[], rating float8);
INSERT INTO walk_claims
SELECT label,laplace_compose(parts),parts,rating FROM (VALUES
 ('sx',ARRAY[pg_temp.leaf(30),pg_temp.leaf(40),pg_temp.leaf(31)],1700::float8),
 ('xg',ARRAY[pg_temp.leaf(31),pg_temp.leaf(40),pg_temp.leaf(33)],1700),
 ('sy',ARRAY[pg_temp.leaf(30),pg_temp.leaf(41),pg_temp.leaf(32)],1300),
 ('yg',ARRAY[pg_temp.leaf(32),pg_temp.leaf(41),pg_temp.leaf(33)],1300)) v(label,parts,rating);
DO $$ BEGIN
 PERFORM pg_temp.place(ARRAY[pg_temp.leaf(30),pg_temp.leaf(31),pg_temp.leaf(32),pg_temp.leaf(33),pg_temp.leaf(40),pg_temp.leaf(41)],0::smallint);
 PERFORM pg_temp.hold(id,parts,1::smallint,true) FROM walk_claims;
END $$;
INSERT INTO consensus SELECT id,rating,60,0.06,1000 FROM walk_claims;
DO $$
DECLARE s blake3:=pg_temp.leaf(30); x blake3:=pg_temp.leaf(31); y blake3:=pg_temp.leaf(32); g blake3:=pg_temp.leaf(33);
        r1 blake3:=pg_temp.leaf(40); r2 blake3:=pg_temp.leaf(41);
        sx blake3; xg blake3; sy blake3; yg blake3; hit record;
BEGIN
 SELECT id INTO sx FROM walk_claims WHERE label='sx'; SELECT id INTO xg FROM walk_claims WHERE label='xg';
 SELECT id INTO sy FROM walk_claims WHERE label='sy'; SELECT id INTO yg FROM walk_claims WHERE label='yg';
 SELECT * INTO hit FROM laplace_pull(ARRAY[s],ARRAY[g]) WHERE goal;
 IF hit IS NULL OR hit.chain<>ARRAY[s,x,g] OR hit.claims<>ARRAY[sx,xg]
 OR hit.via<>x OR hit.claim<>xg OR hit.relation<>r1 OR hit.rating<>1700 OR hit.deviation<>60 OR hit.seed<>s OR hit.hops<>2
 OR hit.cost<=(SELECT cost FROM laplace_pull(ARRAY[s],ARRAY[g]) WHERE id=x) THEN
  RAISE EXCEPTION 'Native pull did not take the cheapest chain to its goal';
 END IF;
 IF (SELECT array_agg(id ORDER BY ordinal) FROM laplace_pull(ARRAY[s],ARRAY[g]))<>ARRAY[s,x,y,g]
 OR (SELECT count(*) FROM laplace_pull(ARRAY[s],ARRAY[g]) WHERE ordinal=1 AND id=s AND hops=0 AND cost=0
       AND via IS NULL AND claim IS NULL AND relation IS NULL AND chain=ARRAY[s] AND claims='{}')<>1
 OR (SELECT cost FROM laplace_pull(ARRAY[s]) WHERE id=x)>=(SELECT cost FROM laplace_pull(ARRAY[s]) WHERE id=y) THEN
  RAISE EXCEPTION 'Native pull lost its close order or the seed row';
 END IF;
 IF (SELECT chain FROM laplace_pull(ARRAY[s],ARRAY[g],middle=>ARRAY[r2]) WHERE goal)<>ARRAY[s,y,g]
 OR EXISTS(SELECT FROM laplace_pull(ARRAY[s],middle=>ARRAY[r2]) WHERE id=x)
 OR EXISTS(SELECT FROM laplace_pull(ARRAY[s],hops=>1) WHERE id=g)
 OR (SELECT count(*) FROM laplace_pull(ARRAY[s],nodes=>2))<>2
 OR (SELECT count(*) FROM laplace_pull(ARRAY[r1]))<>1
 OR EXISTS(SELECT FROM laplace_pull(ARRAY[s],refuse=>ARRAY[r1]) WHERE id=x)
 OR EXISTS(SELECT FROM laplace_pull('{}'::blake3[]))
 OR EXISTS(SELECT FROM laplace_pull(NULL::blake3[])) THEN
  RAISE EXCEPTION 'Native pull lost a middle, hop, node, refusal or empty bound';
 END IF;
 -- fan 1: the seed crosses its lowest claim only; what it reaches holds two and is a hub.
 IF (SELECT count(*) FROM laplace_pull(ARRAY[s],fan=>1))<>2
 OR (SELECT claims[1] FROM laplace_pull(ARRAY[s],fan=>1) WHERE hops=1)<>LEAST(sx,sy)
 OR NOT (SELECT hub FROM laplace_pull(ARRAY[s],fan=>1) WHERE hops=1)
 OR (SELECT hub FROM laplace_pull(ARRAY[s],fan=>1) WHERE hops=0) THEN
  RAISE EXCEPTION 'Native pull crossed a hub or did not cross a seed through its fan';
 END IF;
 BEGIN PERFORM laplace_pull(ARRAY[s],hops=>-1); RAISE EXCEPTION 'negative hops accepted';
 EXCEPTION WHEN invalid_parameter_value THEN NULL; END;

 IF (SELECT array_agg(entity||':'||claim||':'||relation||':'||depth ORDER BY depth)
     FROM laplace_chains(ARRAY[s],ARRAY[r1,r1],ARRAY[1,1],ARRAY[0,1],ARRAY[false,false]))
    <>ARRAY[x||':'||sx||':'||r1||':0',g||':'||xg||':'||r1||':1']
 OR (SELECT array_agg(entity ORDER BY depth) FROM laplace_chains(ARRAY[g],ARRAY[r2,r2],ARRAY[1,1],ARRAY[0,1],ARRAY[true,true]))
    <>ARRAY[y,s]
 OR EXISTS(SELECT FROM laplace_chains(ARRAY[s],ARRAY[r1,r2],ARRAY[1,1],ARRAY[0,1],ARRAY[false,false]))
 OR EXISTS(SELECT FROM laplace_chains(ARRAY[s],ARRAY[r1],ARRAY[1],ARRAY[0],ARRAY[true]))
 OR (SELECT array_agg(DISTINCT root||':'||program||':'||entity::text ORDER BY root||':'||program||':'||entity::text)
     FROM laplace_chains(ARRAY[g,s],ARRAY[r2,r1],ARRAY[1,2],ARRAY[0,0],ARRAY[true,false]))
    <>ARRAY['1:1:'||y::text,'2:2:'||x::text]
 OR (SELECT count(DISTINCT path)||':'||min(path)||':'||max(path)
     FROM laplace_chains(ARRAY[g,s],ARRAY[r2,r1],ARRAY[1,2],ARRAY[0,0],ARRAY[true,false]))<>'2:0:1'
 OR (SELECT count(*) FROM laplace_chains(ARRAY[s],ARRAY[r1,r1],ARRAY[1,1],ARRAY[0,1],ARRAY[false,false])
     WHERE rating=1700 AND deviation=60 AND matches=1000)<>2 THEN
  RAISE EXCEPTION 'Native chains lost a step, a direction, completeness or a root';
 END IF;
 BEGIN PERFORM laplace_chains(ARRAY[s,g],ARRAY[r1],ARRAY[1],ARRAY[0],ARRAY[false],1); RAISE EXCEPTION 'cap ignored';
 EXCEPTION WHEN program_limit_exceeded THEN NULL; END;
 BEGIN PERFORM laplace_chains(ARRAY[s],ARRAY[r1],ARRAY[1,2],ARRAY[0],ARRAY[false]); RAISE EXCEPTION 'ragged arrays accepted';
 EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
ROLLBACK;
