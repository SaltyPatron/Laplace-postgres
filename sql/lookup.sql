-- The lookups ingestion itself needs, present from the start: deduplication asks which IDs are recorded (a file
-- already recorded is found the same way, by the ID of what its metadata says of its bytes), and an incoming
-- attestation asks who attested its claim before. The container and 4D indexes are built after a bulk load
-- (indexes.sql).
CREATE INDEX IF NOT EXISTS entity_id ON entity (id);
CREATE INDEX IF NOT EXISTS physicality_entity ON physicality (entity);
CREATE INDEX IF NOT EXISTS attestation_claim ON attestation (claim);
