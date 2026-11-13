\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
-- The chess perf-cache the deployment configures (laplace.chess; Laplace-Engine's laplace chess writes it from the
-- openings source). Each record it is asked for is checked against the extension's own composition of the same
-- content: chess's layout (Laplace-Engine src/chess.c) composed here from codepoints with laplace_compose, never a
-- constant copied from the module. Everything runs in a rolled-back transaction; nothing is written.
BEGIN;
CREATE FUNCTION pg_temp.cp(text) RETURNS blake3 LANGUAGE sql IMMUTABLE AS $$ SELECT laplace_cp_id(ascii($1)) $$;
CREATE FUNCTION pg_temp.sq(text) RETURNS blake3 LANGUAGE sql IMMUTABLE
  AS $$ SELECT laplace_compose(ARRAY[pg_temp.cp(substr($1,1,1)), pg_temp.cp(substr($1,2,1))]) $$;
CREATE FUNCTION pg_temp.ps(text) RETURNS blake3 LANGUAGE sql IMMUTABLE
  AS $$ SELECT laplace_compose(ARRAY[pg_temp.cp(substr($1,1,1)), pg_temp.sq(substr($1,2,2))]) $$;
CREATE FUNCTION pg_temp.num(text) RETURNS blake3 LANGUAGE sql IMMUTABLE
  AS $$ SELECT laplace_compose(ARRAY(SELECT pg_temp.cp(d) FROM regexp_split_to_table($1,'') WITH ORDINALITY t(d,o) ORDER BY o)) $$;
-- [board, side, castling, en passant], the board its [piece, square]s in square order
CREATE FUNCTION pg_temp.position(text[], text, text, text, text) RETURNS blake3 LANGUAGE sql IMMUTABLE
  AS $$ SELECT laplace_compose(ARRAY[laplace_compose(ARRAY(SELECT pg_temp.ps(p) FROM unnest($1) WITH ORDINALITY t(p,o) ORDER BY o)),
                                     pg_temp.cp($2), laplace_compose(ARRAY[pg_temp.num($3), pg_temp.num($4)]), pg_temp.cp($5)]) $$;
CREATE TEMP TABLE chess_case AS SELECT
  pg_temp.position(ARRAY['Ra1','Nb1','Bc1','Qd1','Ke1','Bf1','Ng1','Rh1','Pa2','Pb2','Pc2','Pd2','Pe2','Pf2','Pg2','Ph2',
                         'pa7','pb7','pc7','pd7','pe7','pf7','pg7','ph7','ra8','nb8','bc8','qd8','ke8','bf8','ng8','rh8'], 'w', '137', '137', '-') AS start,
  pg_temp.position(ARRAY['Ra1','Nb1','Bc1','Qd1','Ke1','Bf1','Ng1','Rh1','Pa2','Pb2','Pc2','Pd2','Pf2','Pg2','Ph2','Pe4',
                         'pa7','pb7','pc7','pd7','pe7','pf7','pg7','ph7','ra8','nb8','bc8','qd8','ke8','bf8','ng8','rh8'], 'b', '137', '137', '-') AS after_e4,
  laplace_compose(ARRAY[pg_temp.cp('P'), pg_temp.sq('e2'), pg_temp.sq('e4')]) AS e2e4,
  laplace_id('no such chess content') AS nothing;

-- the module is configured, loads, and is bound to this tier 0
SELECT laplace_chess_fingerprint() ~ '^[0-9a-f]{64}$';
-- the vocabulary by direct address: e4 is square 28, Ke1 piece 5 (K) of PNBRQKpnbrqk at e1, 137 the number 137
SELECT (SELECT id FROM laplace_chess_at('square', ARRAY[28])) = pg_temp.sq('e4')
   AND (SELECT id FROM laplace_chess_at('piece_square', ARRAY[5 * 64 + 4])) = pg_temp.ps('Ke1')
   AND (SELECT id FROM laplace_chess_at('number', ARRAY[137])) = pg_temp.num('137')
   AND NOT EXISTS (SELECT FROM laplace_chess_at('square', ARRAY[64, -1]));
-- a record by ID: its section, slot and tier, and the real coordinate and Hilbert key the extension gives e4's text
SELECT r.section = 'square' AND r.slot = 28 AND r.tier = 1
   AND ST_AsEWKB(r.coord) = ST_AsEWKB(laplace_coord('e4')) AND r.hilbert = laplace_hilbert('e4')
  FROM laplace_chess_records(ARRAY[pg_temp.sq('e4')]) r;
-- the admitted positions and the move between them, and a request's order kept by ordinal, a miss no row
SELECT string_agg(r.ordinal || ':' || r.section || ':' || r.tier, ' ' ORDER BY r.ordinal)
  FROM chess_case c, laplace_chess_records(ARRAY[c.nothing, c.start, c.e2e4, c.after_e4]) r;
-- (position, move) -> position, and a move that is not played there is no row
SELECT (SELECT next FROM laplace_chess_next(ARRAY[c.start], ARRAY[c.e2e4])) = c.after_e4
   AND NOT EXISTS (SELECT FROM laplace_chess_next(ARRAY[c.after_e4, c.nothing], ARRAY[c.e2e4, c.e2e4]))
  FROM chess_case c;
ROLLBACK;
SELECT 'chess: the configured perf-cache''s records are the extension''s own compositions; misses are no rows';
