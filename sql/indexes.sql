-- Indexes, built after a bulk load. Each is created on the partitioned parent, which builds one per partition.
-- GIN over each path's packed IDs (laplace_path_ops) finds containers exactly; GiST on the real coordinates serves 4D
-- nearest-neighbor; the semantics tables are indexed by claim and witness.
\timing on
SET maintenance_work_mem = '8GB';
SET max_parallel_maintenance_workers = 6;
CREATE INDEX entity_id ON entity (id);
CREATE INDEX entity_hilbert ON entity (hilbert);
CREATE INDEX entity_coord ON entity USING gist (coord gist_geometry_ops_nd);
CREATE INDEX physicality_entity ON physicality (entity);
CREATE INDEX physicality_paths ON physicality USING gin (path laplace_path_ops);
CREATE INDEX source_trunk ON source (trunk);
CREATE INDEX attestation_claim ON attestation (claim);
CREATE INDEX attestation_witness ON attestation (witness);
ANALYZE;
