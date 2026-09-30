-- Laplace 1.2 -> 1.3: the schema is the extension's; the mask; the web's reads take the bits a row must have.
\echo Use "ALTER EXTENSION laplace UPDATE" to load this file. \quit

-- ------------------------------------------------------------------------------------------------ the mask
-- A row's mask (Semantics: Claims, Masks): 256 bits, what the row is (bits 0 to 7: a claim, a record, a tuple, a
-- file) and the types it holds, by the highway's layout. GIN keys are the positions of its set bits.
CREATE FUNCTION laplace_mask_has(bit, smallint) RETURNS boolean AS 'MODULE_PATHNAME', 'laplace_mask_has' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_has_all(bit, smallint[]) RETURNS boolean AS 'MODULE_PATHNAME', 'laplace_mask_has_all' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_has_any(bit, smallint[]) RETURNS boolean AS 'MODULE_PATHNAME', 'laplace_mask_has_any' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR ? (LEFTARG = bit, RIGHTARG = smallint, FUNCTION = laplace_mask_has, RESTRICT = contsel, JOIN = contjoinsel);
CREATE OPERATOR ?& (LEFTARG = bit, RIGHTARG = smallint[], FUNCTION = laplace_mask_has_all, RESTRICT = contsel, JOIN = contjoinsel);
CREATE OPERATOR ?| (LEFTARG = bit, RIGHTARG = smallint[], FUNCTION = laplace_mask_has_any, RESTRICT = contsel, JOIN = contjoinsel);
CREATE FUNCTION laplace_mask_extract_value(bit, internal, internal) RETURNS internal AS 'MODULE_PATHNAME', 'laplace_mask_extract_value' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_extract_query(internal, internal, int2, internal, internal, internal, internal) RETURNS internal AS 'MODULE_PATHNAME', 'laplace_mask_extract_query' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_consistent(internal, int2, internal, int4, internal, internal, internal, internal) RETURNS boolean AS 'MODULE_PATHNAME', 'laplace_mask_consistent' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_triconsistent(internal, int2, internal, int4, internal, internal, internal) RETURNS "char" AS 'MODULE_PATHNAME', 'laplace_mask_triconsistent' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR CLASS laplace_mask_ops FOR TYPE bit USING gin AS
  OPERATOR 1 ? (bit, smallint),
  OPERATOR 2 ?& (bit, smallint[]),
  OPERATOR 3 ?| (bit, smallint[]),
  FUNCTION 1 btint2cmp(smallint, smallint),
  FUNCTION 2 laplace_mask_extract_value(bit, internal, internal),
  FUNCTION 3 laplace_mask_extract_query(internal, internal, int2, internal, internal, internal, internal),
  FUNCTION 4 laplace_mask_consistent(internal, int2, internal, int4, internal, internal, internal, internal),
  FUNCTION 6 laplace_mask_triconsistent(internal, int2, internal, int4, internal, internal, internal),
  STORAGE smallint;
-- The mask bit of a type, from its content or its ID: the highway's field for its list, plus its slot; -1 when none.
CREATE FUNCTION laplace_mask_bit(text) RETURNS smallint AS 'MODULE_PATHNAME', 'laplace_mask_bit' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_bit(blake3) RETURNS smallint AS 'MODULE_PATHNAME', 'laplace_mask_bit_of' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- ------------------------------------------------------------------------------------------------ the tables
-- The five tables, the extension's own (Storage: Physicality; Semantics), so a database is a Laplace database by
-- CREATE EXTENSION laplace, and pg_dump carries their data. An ID is a blake3. Entities and physicalities partition
-- by tier, and the tiers measured largest again 16 ways by the first hex digit of the ID (an ID is a hash, so the
-- parts fill evenly and a lookup by ID prunes to one). Hilbert values are unsigned 64-bit stored with the top bit
-- flipped, so bigint order is Hilbert order.
CREATE TABLE IF NOT EXISTS entity (
  id       blake3   NOT NULL,
  tier     smallint NOT NULL,
  coord    geometry(PointZM) NOT NULL,           -- the real 4D coordinate; M is W
  hilbert  bigint   NOT NULL
) PARTITION BY LIST (tier);
CREATE TABLE IF NOT EXISTS physicality (
  entity   blake3   NOT NULL,
  tier     smallint NOT NULL,
  hilbert  bigint   NOT NULL,
  path     geometry NOT NULL,                    -- the constituents' IDs in X/Y/Z, each vertex's M its run and what it is
  mask     bit(256) NOT NULL DEFAULT B'0'::bit(256)   -- what the row is, and the types it holds (laplace_mask_ops)
) PARTITION BY LIST (tier);
ALTER TABLE physicality ADD COLUMN IF NOT EXISTS mask bit(256) NOT NULL DEFAULT B'0'::bit(256);
DO $$
DECLARE t int; k int; lo text; hi text; big int[] := ARRAY[0, 2, 3, 4, 5, 6]; r record;
BEGIN
  FOR t IN 0..15 LOOP
    IF t = ANY (big) THEN
      EXECUTE format('CREATE TABLE IF NOT EXISTS entity_t%s PARTITION OF entity FOR VALUES IN (%s) PARTITION BY RANGE (id)', t, t);
      EXECUTE format('CREATE TABLE IF NOT EXISTS physicality_t%s PARTITION OF physicality FOR VALUES IN (%s) PARTITION BY RANGE (entity)', t, t);
      FOR k IN 0..15 LOOP
        lo := CASE WHEN k = 0 THEN 'MINVALUE' ELSE quote_literal(to_hex(k) || repeat('0', 31)) END;
        hi := CASE WHEN k = 15 THEN 'MAXVALUE' ELSE quote_literal(to_hex(k + 1) || repeat('0', 31)) END;
        EXECUTE format('CREATE TABLE IF NOT EXISTS entity_t%s_%s PARTITION OF entity_t%s FOR VALUES FROM (%s) TO (%s)', t, to_hex(k), t, lo, hi);
        EXECUTE format('CREATE TABLE IF NOT EXISTS physicality_t%s_%s PARTITION OF physicality_t%s FOR VALUES FROM (%s) TO (%s)', t, to_hex(k), t, lo, hi);
      END LOOP;
    ELSE
      EXECUTE format('CREATE TABLE IF NOT EXISTS entity_t%s PARTITION OF entity FOR VALUES IN (%s)', t, t);
      EXECUTE format('CREATE TABLE IF NOT EXISTS physicality_t%s PARTITION OF physicality FOR VALUES IN (%s)', t, t);
    END IF;
  END LOOP;
  CREATE TABLE IF NOT EXISTS entity_tx PARTITION OF entity DEFAULT;
  CREATE TABLE IF NOT EXISTS physicality_tx PARTITION OF physicality DEFAULT;
  -- a path's X/Y/Z are packed IDs, not positions: no statistics on them; container lookups take a millisecond, so no parallel workers per partition
  ALTER TABLE physicality ALTER COLUMN path SET STATISTICS 0;
  FOR r IN SELECT c.relname FROM pg_class c JOIN pg_inherits i ON i.inhrelid = c.oid WHERE c.relname LIKE 'physicality_t%' AND c.relkind = 'r' LOOP
    EXECUTE format('ALTER TABLE %I SET (parallel_workers = 0)', r.relname);
  END LOOP;
END $$;
CREATE TABLE IF NOT EXISTS witness (
  id       blake3 PRIMARY KEY,                     -- an entity: whatever testifies, named as content
  lineage  blake3,                                 -- the witness it derives from, so copies are not independent
  trust    double precision NOT NULL             -- -1 .. 1: MANDATE is 1, no information 0, reliably wrong -1
);
CREATE TABLE IF NOT EXISTS attestation (         -- the ledger: append-only, in reading order
  claim    blake3 NOT NULL,
  witness  blake3 NOT NULL,
  score    real NOT NULL,                        -- win 1, draw 0.5, loss 0, or a score between
  position integer                               -- the place a witness gave the claim among its like, as given
);
CREATE TABLE IF NOT EXISTS consensus (           -- updated in place as matchups are played
  claim       blake3 PRIMARY KEY,
  rating      double precision NOT NULL,
  deviation   double precision NOT NULL,
  volatility  double precision NOT NULL,
  matches     integer NOT NULL
) WITH (fillfactor = 80);
-- the tables' data goes with a dump (tables a database had before this version are made the extension's by laplace deploy)
DO $$
DECLARE r record;
BEGIN
  FOR r IN SELECT c.oid::regclass AS t FROM pg_class c WHERE c.relkind = 'r' AND (c.relname ~ '^(entity|physicality)_t[0-9a-fx]+(_[0-9a-f])?$' OR c.relname IN ('witness', 'attestation', 'consensus')) LOOP
    PERFORM pg_catalog.pg_extension_config_dump(r.t, '');
  END LOOP;
END $$;
-- The indexes, all of them from the start; laplace index makes them again if one was dropped. Each is created on
-- the partitioned parent, which builds one per partition: IDs (the lookups deduplication and the ledger need), the
-- Hilbert order, the real coordinates (GiST, 4D), the containers and the mask together (GIN), the ledger by claim
-- and by witness.
CREATE FUNCTION laplace_schema_indexes() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  CREATE INDEX IF NOT EXISTS entity_id ON entity (id);
  CREATE INDEX IF NOT EXISTS physicality_entity ON physicality (entity);
  CREATE INDEX IF NOT EXISTS attestation_claim ON attestation (claim);
  CREATE INDEX IF NOT EXISTS entity_hilbert ON entity (hilbert);
  CREATE INDEX IF NOT EXISTS entity_coord ON entity USING gist (coord gist_geometry_ops_nd);
  CREATE INDEX IF NOT EXISTS physicality_paths ON physicality USING gin (path laplace_path_ops, mask laplace_mask_ops);
  CREATE INDEX IF NOT EXISTS attestation_witness ON attestation (witness);
END $$;
DROP INDEX IF EXISTS physicality_paths;       -- the container index without the mask, where a database had it
SELECT laplace_schema_indexes();

-- ------------------------------------------------------------------------------------------------ the web
-- The web's reads take, besides the parts, the mask bits a row must have: laplace_mask_bit('claim') for claims,
-- a type's bit for claims of that type; an empty array asks for any row.
DROP FUNCTION laplace_claims(blake3[], bigint);
DROP FUNCTION laplace_claims_each(blake3[], bigint);
DROP FUNCTION laplace_containers(blake3[]);
CREATE FUNCTION laplace_claims(parts blake3[], fan bigint, bits smallint[], OUT entity blake3, OUT path geometry, OUT rating double precision, OUT deviation double precision, OUT volatility double precision, OUT matches integer) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_claims' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 64;
CREATE FUNCTION laplace_claims_each(ids blake3[], fan bigint, bits smallint[], OUT i bigint, OUT entity blake3, OUT path geometry, OUT rating double precision, OUT deviation double precision, OUT volatility double precision, OUT matches integer) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_claims_each' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 1024;
CREATE FUNCTION laplace_containers(parts blake3[], bits smallint[], OUT entity blake3, OUT path geometry, OUT tier smallint, OUT mask bit) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_containers' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 256;

-- a type by the key a resource points at it with (from the keys beside the highway)
CREATE FUNCTION laplace_type_key(list text, key text) RETURNS integer
  AS 'MODULE_PATHNAME', 'laplace_type_key' LANGUAGE C STABLE STRICT PARALLEL SAFE;
