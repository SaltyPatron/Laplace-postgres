\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
-- Up the container index from keys to the files that hold them, in one call:
-- through content only, never through a file's record, stopping at files.
BEGIN;
CREATE TEMP TABLE held_words AS SELECT unnest(ARRAY['alpha','beta','gamma','delta','one','two','three','source']) AS content;
INSERT INTO entity(id,tier,coord,hilbert)
SELECT laplace_id(content),laplace_tier(content),laplace_coord(content),laplace_hilbert(content) FROM held_words;
INSERT INTO physicality(entity,tier,hilbert,path)
SELECT laplace_id(content),laplace_tier(content),laplace_hilbert(content),
  laplace_path(ARRAY(SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table(content,'') c)) FROM held_words;
CREATE FUNCTION pg_temp.held_node(parts blake3[],metadata bigint[] DEFAULT NULL) RETURNS blake3
LANGUAGE plpgsql AS $$
DECLARE composed blake3:=laplace_compose(parts); point geometry; path geometry:=laplace_path(parts);
        coord geometry; altitude smallint; hilbert bigint;
BEGIN
  SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(coalesce(e.coord,ST_GeomFromEWKB(laplace_cp_coord_ewkb(laplace_codepoint(p)))))),
         (max(coalesce(e.tier,0))+1)::smallint
    INTO coord,altitude FROM unnest(parts) p LEFT JOIN entity e ON e.id=p;
  hilbert:=laplace_hilbert4(coord) # '-9223372036854775808'::bigint;
  IF metadata IS NOT NULL THEN
    FOR i IN 1..cardinality(metadata) LOOP
      point:=ST_PointN(path,i);
      path:=ST_SetPoint(path,i-1,ST_MakePoint(ST_X(point),ST_Y(point),ST_Z(point),ST_M(point)+metadata[i]));
    END LOOP;
  END IF;
  INSERT INTO entity(id,tier,coord,hilbert) VALUES(composed,altitude,coord,hilbert) ON CONFLICT DO NOTHING;
  INSERT INTO physicality(entity,tier,hilbert,path) VALUES(composed,altitude,hilbert,path) ON CONFLICT DO NOTHING;
  RETURN composed;
END $$;
DO $$
DECLARE alpha blake3:=laplace_id('alpha'); beta blake3:=laplace_id('beta'); gamma blake3:=laplace_id('gamma');
        delta blake3:=laplace_id('delta'); one blake3:=laplace_id('one');
        meta bigint:=4294967296;  -- LP_SAID_METADATA in a vertex's M
        first_line blake3; second_line blake3; body blake3; a blake3; b blake3; c blake3; trunk blake3;
BEGIN
  first_line:=pg_temp.held_node(ARRAY[alpha,beta]);
  second_line:=pg_temp.held_node(ARRAY[gamma,delta]);
  body:=pg_temp.held_node(ARRAY[first_line,second_line]);
  -- each file's record names it with words of its own; file a's record also holds alpha
  a:=pg_temp.held_node(ARRAY[pg_temp.held_node(ARRAY[one,alpha]),body],ARRAY[meta,0]);
  b:=pg_temp.held_node(ARRAY[pg_temp.held_node(ARRAY[laplace_id('two'),one]),second_line],ARRAY[meta,0]);
  c:=pg_temp.held_node(ARRAY[pg_temp.held_node(ARRAY[laplace_id('three'),one]),first_line],ARRAY[meta,0]);
  trunk:=pg_temp.held_node(ARRAY[laplace_id('source'),pg_temp.held_node(ARRAY[a,b])]);
  IF (SELECT array_agg(file::text||':'||keys ORDER BY file::text) FROM laplace_held_files(ARRAY[alpha,gamma]))
       IS DISTINCT FROM (SELECT array_agg(f ORDER BY f) FROM unnest(ARRAY[a::text||':3',b::text||':2',c::text||':1']) f)
  OR EXISTS(SELECT FROM laplace_held_files(ARRAY[alpha,gamma]) WHERE hubs<>0 OR cut OR levels<3) THEN
    RAISE EXCEPTION 'Native held files: content walk changed';
  END IF;
  -- a trunk keeps the files under its spine
  IF (SELECT array_agg(file::text ORDER BY file::text) FROM laplace_held_files(ARRAY[alpha,gamma],trunk))
       IS DISTINCT FROM (SELECT array_agg(f ORDER BY f) FROM unnest(ARRAY[a::text,b::text]) f) THEN
    RAISE EXCEPTION 'Native held files: trunk scope changed';
  END IF;
  -- a name held only by files' records is no occurrence; an absent key reaches nothing
  IF (SELECT count(*) FROM laplace_held_files(ARRAY[one]) WHERE file IS NULL AND keys=0)<>1
  OR (SELECT count(*) FROM laplace_held_files(ARRAY[one]))<>1
  OR (SELECT count(*) FROM laplace_held_files(ARRAY['00000000000000000000000000000000'::blake3]) WHERE file IS NULL)<>1 THEN
    RAISE EXCEPTION 'Native held files: a record or an absent key reached a file';
  END IF;
  -- a key with more direct holders than the cap is a hub and is not walked
  IF (SELECT array_agg(file::text||':'||keys||'/'||hubs ORDER BY file::text) FROM laplace_held_files(ARRAY[alpha,gamma],NULL,1))
       IS DISTINCT FROM (SELECT array_agg(f ORDER BY f) FROM unnest(ARRAY[a::text||':2/1',b::text||':2/1']) f)
  OR (SELECT array_agg(file::text||':'||keys||'/'||hubs ORDER BY file::text) FROM laplace_held_files(ARRAY[beta],NULL,1))
       IS DISTINCT FROM (SELECT array_agg(f ORDER BY f) FROM unnest(ARRAY[a::text||':1/0',c::text||':1/0']) f) THEN
    RAISE EXCEPTION 'Native held files: hub cap changed';
  END IF;
  -- the walk is bounded: past max_nodes it stops and says so
  IF NOT (SELECT bool_and(cut) FROM laplace_held_files(ARRAY[alpha,gamma],NULL,-1,3)) THEN
    RAISE EXCEPTION 'Native held files: max_nodes did not bound the walk';
  END IF;
END $$;
ROLLBACK;
