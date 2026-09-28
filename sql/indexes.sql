-- Indexes, built after a bulk load. Each is created on the partitioned parent, which builds one per partition.
-- GIN on the IDs decoded from each path finds containers; GiST on the real coordinates serves 4D nearest-neighbor.
\timing on
SET maintenance_work_mem = '8GB';
SET max_parallel_maintenance_workers = 6;
CREATE INDEX entity_id ON entity (id);
CREATE INDEX entity_hilbert ON entity (hilbert);
CREATE INDEX entity_coord ON entity USING gist (coord gist_geometry_ops_nd);
CREATE INDEX physicality_entity ON physicality (entity);
CREATE INDEX physicality_containers ON physicality USING gin (laplace_vertex_ids(path));
CREATE INDEX entity_stats_id ON entity_stats (id);
CREATE INDEX entity_stats_occurrences ON entity_stats (tier, occurrences DESC);
CREATE INDEX source_trunk ON source (trunk);
ANALYZE;
