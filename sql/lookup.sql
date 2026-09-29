-- The lookups ingestion itself needs, present from the start: deduplication asks which IDs are recorded, and a file
-- already recorded is found by the hash of its bytes. The container and 4D indexes are built after a bulk load
-- (indexes.sql).
CREATE INDEX IF NOT EXISTS entity_id ON entity (id);
CREATE INDEX IF NOT EXISTS physicality_entity ON physicality (entity);
CREATE INDEX IF NOT EXISTS source_content ON source (content);
CREATE INDEX IF NOT EXISTS source_trunk ON source (trunk);
