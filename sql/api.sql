-- Current versioned Native API: one declaration for install, upgrade and repair.
CREATE OR REPLACE FUNCTION @extschema@.laplace_testimony(claims @extschema@.blake3[], sources @extschema@.blake3[] DEFAULT NULL,
    OUT claim @extschema@.blake3, OUT source @extschema@.blake3, OUT context @extschema@.blake3,
    OUT ordinal bigint, OUT outcome smallint, OUT occurrences bigint, OUT tokens bigint,
    OUT score double precision, OUT "position" integer, OUT trust double precision,
    OUT dependence_root @extschema@.blake3, OUT calculation boolean) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 256 PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_testimony';

