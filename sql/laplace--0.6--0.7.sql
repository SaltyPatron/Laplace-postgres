-- Laplace-postgres 0.6 → 0.7: the flags that go with tier 0. What the Unicode Standard says of a codepoint, read from
-- the memory-mapped flags by the standard's own names; no table is read.
\echo Use "ALTER EXTENSION laplace UPDATE TO '0.7'" to load this file. \quit

-- The codepoint an atom is, or NULL for an entity that is not an atom.
CREATE FUNCTION laplace_codepoint(uuid) RETURNS integer
  AS 'MODULE_PATHNAME', 'laplace_codepoint' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- A codepoint's 256 bits.
CREATE FUNCTION laplace_flags(integer) RETURNS bytea
  AS 'MODULE_PATHNAME', 'laplace_cp_flags' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- What a property is, of a codepoint or of an atom: laplace_said(65, 'General_Category') is Uppercase_Letter.
CREATE FUNCTION laplace_said(integer, text) RETURNS text
  AS 'MODULE_PATHNAME', 'laplace_cp_said' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_said(uuid, text) RETURNS text
  AS 'MODULE_PATHNAME', 'laplace_said' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- Whether a property has a value: laplace_is(id, 'script', 'Greek'). Names are matched by the standard's rule.
CREATE FUNCTION laplace_is(integer, text, text) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_cp_is' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_is(uuid, text, text) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_is' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
