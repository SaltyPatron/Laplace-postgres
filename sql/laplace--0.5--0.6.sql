-- Laplace-postgres 0.5 → 0.6: a text's entity computed in place by the one decomposition of text (UAX #29), the shape
-- measures beside Fréchet, a standing's confidence, and the fingerprint of the tier 0 in use.
\echo Use "ALTER EXTENSION laplace UPDATE TO '0.6'" to load this file. \quit

-- The entity of a text: what the engine records for the same text. laplace_id('Sherlock Holmes') is the ID of
-- [[S,h,e,r,l,o,c,k], ' ', [H,o,l,m,e,s]]. (laplace_text_id composes a text's codepoints directly, as one word.)
CREATE FUNCTION laplace_id(text) RETURNS uuid
  AS 'MODULE_PATHNAME', 'laplace_id' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_tier(text) RETURNS smallint
  AS 'MODULE_PATHNAME', 'laplace_tier' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_coord_ewkb(text) RETURNS bytea
  AS 'MODULE_PATHNAME', 'laplace_coord_ewkb' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_coord(text) RETURNS geometry
  AS $$ SELECT ST_GeomFromEWKB(laplace_coord_ewkb($1)) $$ LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_hilbert(text) RETURNS bigint
  AS 'MODULE_PATHNAME', 'laplace_hilbert' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- Its constituents in order, repeats included: the phrase to look for inside paths (path @> laplace_parts('...')).
CREATE FUNCTION laplace_parts(text) RETURNS uuid[]
  AS 'MODULE_PATHNAME', 'laplace_parts' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Shape measures on real coordinates, each for its purpose.
CREATE FUNCTION laplace_frechet4d(geometry, geometry, integer) RETURNS double precision      -- up to k vertices of each skipped
  AS 'MODULE_PATHNAME', 'laplace_frechet4d_outliers' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_dtw4d(geometry, geometry) RETURNS double precision
  AS 'MODULE_PATHNAME', 'laplace_dtw4d' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_edr4d(geometry, geometry, double precision) RETURNS bigint           -- edits, vertices within eps equal
  AS 'MODULE_PATHNAME', 'laplace_edr4d' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- How hard a strand tugs back: a standing's chance of beating the anchor, read k deviations below its rating.
CREATE FUNCTION laplace_confidence(rating double precision, deviation double precision, k double precision DEFAULT 2)
  RETURNS double precision AS 'MODULE_PATHNAME', 'laplace_confidence' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Which tier 0 this database computes with: two installs with one fingerprint give the same content the same coordinates.
CREATE FUNCTION laplace_fingerprint() RETURNS text
  AS 'MODULE_PATHNAME', 'laplace_fingerprint' LANGUAGE C STABLE PARALLEL SAFE;
