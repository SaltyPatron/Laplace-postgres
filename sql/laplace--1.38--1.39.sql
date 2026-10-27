\echo Use "ALTER EXTENSION laplace UPDATE TO '1.39'" to load this file. \quit

-- Decode the ordered endpoint with Native's packed/RLE path reader. The
-- constituent GIN key set deliberately carries neither order nor repetition.
CREATE FUNCTION laplace_path_last(geometry) RETURNS blake3
AS 'MODULE_PATHNAME','laplace_path_last'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

SELECT laplace_restore_function_bindings();
