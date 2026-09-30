-- The indexes. laplace deploy makes them all from the start and every load keeps them; laplace index makes them again. Each is created on the partitioned parent, which builds one per partition.
-- GIN over each path's packed IDs (laplace_path_ops) finds containers exactly; GiST on the real coordinates serves 4D
-- nearest-neighbor; the semantics tables are indexed by claim and witness. The ID lookups are in lookup.sql.
\timing on
SET maintenance_work_mem = '8GB';
SET max_parallel_maintenance_workers = 6;
CREATE INDEX IF NOT EXISTS entity_hilbert ON entity (hilbert);
CREATE INDEX IF NOT EXISTS entity_coord ON entity USING gist (coord gist_geometry_ops_nd);
CREATE INDEX IF NOT EXISTS physicality_paths ON physicality USING gin (path laplace_path_ops);
CREATE INDEX IF NOT EXISTS attestation_claim ON attestation (claim);
CREATE INDEX IF NOT EXISTS attestation_witness ON attestation (witness);
ANALYZE;
