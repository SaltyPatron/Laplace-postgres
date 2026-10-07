-- 1.12: provenance is containment (Engine#22, Engine#39 step 2). Who said a claim, how many times and how is a walk up
-- the container index from the claim to the records that say it, the files and the source trunks above them; the
-- Engine reads nothing else. The attestation table and laplace_attested, which read it, are retired.
-- A database seeded by an engine that wrote the table loses what only the table held: seed it again (Laplace-Operations
-- reseeds a database whose stamp differs from the code's).
\echo Use "ALTER EXTENSION laplace UPDATE TO '1.12'" to load this file. \quit

DROP FUNCTION IF EXISTS laplace_attested(blake3[]);
DROP INDEX IF EXISTS attestation_witness;
DROP TABLE IF EXISTS attestation;
CREATE OR REPLACE FUNCTION laplace_schema_indexes() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  CREATE INDEX IF NOT EXISTS entity_hilbert ON entity (hilbert);
  CREATE INDEX IF NOT EXISTS entity_coord ON entity USING gist (coord laplace_point4d_ops);
  -- 32 MB a partition: a search scans the pending list, so it spills around a batch instead of holding a source.
  -- 4 MB spilled every few thousand paths (full-page images). 256 MB held the list until the source ended.
  CREATE INDEX IF NOT EXISTS physicality_paths ON physicality USING gin (path laplace_path_ops, mask laplace_mask_ops) WITH (gin_pending_list_limit = 32768);
END $$;
