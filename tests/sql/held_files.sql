\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
-- Up the container index from keys to the files that hold them, in one call:
-- through content only, never through a file's record, stopping at files.
-- The deployed database holds real words with many holders. Every key here is
-- a word of two leaves unique to this case, so each walk reaches only its rows.
BEGIN;
CREATE FUNCTION pg_temp.leaf(n integer) RETURNS blake3 LANGUAGE sql IMMUTABLE
RETURN md5('Laplace check regression held_files leaf '||n)::blake3;
CREATE TEMP TABLE held_words AS
SELECT content,laplace_compose(ARRAY[pg_temp.leaf(2*n::integer),pg_temp.leaf(2*n::integer+1)]) AS id,
  laplace_path(ARRAY[pg_temp.leaf(2*n::integer),pg_temp.leaf(2*n::integer+1)]) AS path
FROM unnest(ARRAY['alpha','beta','gamma','delta','one','two','three','source','epsilon']) WITH ORDINALITY w(content,n);
CREATE FUNCTION pg_temp.word(label text) RETURNS blake3 LANGUAGE sql STABLE
RETURN (SELECT id FROM held_words WHERE content=label);
INSERT INTO entity(id,tier,coord,hilbert)
SELECT id,1,laplace_coord(content),laplace_hilbert(content) FROM held_words;
INSERT INTO physicality(entity,tier,hilbert,path)
SELECT id,1,laplace_hilbert(content),path FROM held_words;
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
DECLARE alpha blake3:=pg_temp.word('alpha'); beta blake3:=pg_temp.word('beta'); gamma blake3:=pg_temp.word('gamma');
        delta blake3:=pg_temp.word('delta'); one blake3:=pg_temp.word('one');
        meta bigint:=4294967296;  -- LP_SAID_METADATA in a vertex's M
        first_line blake3; second_line blake3; body blake3; a blake3; b blake3; c blake3; d blake3; twice blake3; trunk blake3;
BEGIN
  first_line:=pg_temp.held_node(ARRAY[alpha,beta]);
  second_line:=pg_temp.held_node(ARRAY[gamma,delta]);
  body:=pg_temp.held_node(ARRAY[first_line,second_line]);
  -- each file's record names it with words of its own; file a's record also holds alpha
  a:=pg_temp.held_node(ARRAY[pg_temp.held_node(ARRAY[one,alpha]),body],ARRAY[meta,0]);
  b:=pg_temp.held_node(ARRAY[pg_temp.held_node(ARRAY[pg_temp.word('two'),one]),second_line],ARRAY[meta,0]);
  c:=pg_temp.held_node(ARRAY[pg_temp.held_node(ARRAY[pg_temp.word('three'),one]),first_line],ARRAY[meta,0]);
  -- a source trunk has its record first, said as a file has its OS record
  trunk:=pg_temp.held_node(ARRAY[pg_temp.word('source'),pg_temp.held_node(ARRAY[a,b])],ARRAY[meta,0]);
  IF (SELECT array_agg(file::text||':'||keys ORDER BY file::text) FROM laplace_held_files(ARRAY[alpha,gamma]))
       IS DISTINCT FROM (SELECT array_agg(f ORDER BY f) FROM unnest(ARRAY[a::text||':3',b::text||':2',c::text||':1']) f)
  OR EXISTS(SELECT FROM laplace_held_files(ARRAY[alpha,gamma]) WHERE hubs<>0 OR cut OR levels<3) THEN
    RAISE EXCEPTION 'Native held files: content walk changed';
  END IF;
  -- each file's unit: the lowest node of its content holding every key the file holds, its path length, and how
  -- many times each key stands below it
  IF (SELECT array_agg(file::text||':'||unit::text||':'||unit_length||':'||counts::text||':'||unit_tier ORDER BY file::text) FROM laplace_held_files(ARRAY[alpha,gamma]))
       IS DISTINCT FROM (SELECT array_agg(f ORDER BY f) FROM unnest(ARRAY[a::text||':'||body::text||':2:{1,1}:3',
         b::text||':'||second_line::text||':2:{0,1}:2',c::text||':'||first_line::text||':2:{1,0}:2']) f) THEN
    RAISE EXCEPTION 'Native held files: units changed';
  END IF;
  -- a key said twice in a run stands twice below its unit, and the run counts in the unit's length
  twice:=pg_temp.held_node(ARRAY[pg_temp.word('epsilon'),pg_temp.word('epsilon'),delta]);
  d:=pg_temp.held_node(ARRAY[pg_temp.held_node(ARRAY[pg_temp.word('two'),pg_temp.word('three')]),twice],ARRAY[meta,0]);
  IF (SELECT array_agg(file::text||':'||unit::text||':'||unit_length||':'||counts::text) FROM laplace_held_files(ARRAY[pg_temp.word('epsilon')]))
       IS DISTINCT FROM ARRAY[d::text||':'||twice::text||':3:{2}'] THEN
    RAISE EXCEPTION 'Native held files: occurrence counts changed';
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
