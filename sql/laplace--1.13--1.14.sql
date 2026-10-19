-- One uint64 carrier, shared with Native's manifest/banks.tsv contract.
\echo Use "ALTER EXTENSION laplace UPDATE TO '1.14'" to load this file. \quit

CREATE FUNCTION laplace_mask_slot(bank text, value text) RETURNS bit
  AS 'MODULE_PATHNAME', 'laplace_mask_slot' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_field(bank text) RETURNS bit
  AS 'MODULE_PATHNAME', 'laplace_mask_field' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_value(mask bit, bank text) RETURNS smallint
  AS 'MODULE_PATHNAME', 'laplace_mask_value' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_has_value(mask bit, bank text, value text) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_mask_has_value' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_valid(mask bit) RETURNS boolean
  AS 'MODULE_PATHNAME', 'laplace_mask_valid' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_mask_merge(a bit, b bit) RETURNS bit
  AS 'MODULE_PATHNAME', 'laplace_mask_merge' LANGUAGE C STABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION laplace_mask_slot(text, text) IS
  '64-bit carrier encoding of a frozen bank slot. For exact enum filtering use (mask & laplace_mask_field(bank)) = laplace_mask_slot(bank, value).';
COMMENT ON FUNCTION laplace_mask_merge(bit, bit) IS
  'Native checked bitwise OR: combine disjoint fields and row-role flags; reject conflicting enum values.';

-- Old ingestion wrote the four row-role flags. Unassigned old type bits cannot
-- be reinterpreted as the new enum fields without changing their meaning.
DO $$
BEGIN
  IF EXISTS (SELECT FROM physicality WHERE substring(mask::text FROM 5) ~ '1') THEN
    RAISE EXCEPTION 'legacy carrier contains unassigned type bits; reconstruct these labels from canonical identifiers before upgrading'
      USING ERRCODE = 'data_exception';
  END IF;
END $$;

DROP INDEX IF EXISTS physicality_paths;
ALTER TABLE physicality ALTER COLUMN mask DROP DEFAULT;
ALTER TABLE physicality ALTER COLUMN mask TYPE bit(64)
  USING substring(mask::text FROM 1 FOR 64)::bit(64);
ALTER TABLE physicality ALTER COLUMN mask SET DEFAULT B'0'::bit(64);
ALTER TABLE physicality ADD CONSTRAINT physicality_mask_valid CHECK (laplace_mask_valid(mask));
ALTER TABLE entity ADD COLUMN mask bit(64) NOT NULL DEFAULT B'0'::bit(64)
  CONSTRAINT entity_mask_valid CHECK (laplace_mask_valid(mask));
SELECT laplace_schema_indexes();
