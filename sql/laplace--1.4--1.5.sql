-- Laplace 1.4 -> 1.5: the ledger and the statistics partitioned by the claim's ID hash, as the content is.
\echo Use "ALTER EXTENSION laplace UPDATE" to load this file. \quit

-- A database that has the two as single tables: each is renamed, made anew partitioned, filled from the old as one set,
-- and the old dropped. Storage: Physicality, "Partitions go by the ID hash"; Sequence: Database 8.10.
DO $$
DECLARE k int; lo text; hi text; has_att boolean; has_con boolean;
BEGIN
  SELECT EXISTS (SELECT 1 FROM pg_class WHERE relname = 'attestation' AND relkind = 'r') INTO has_att;
  SELECT EXISTS (SELECT 1 FROM pg_class WHERE relname = 'consensus' AND relkind = 'r') INTO has_con;
  IF has_att THEN
    ALTER EXTENSION laplace DROP TABLE attestation; ALTER TABLE attestation RENAME TO attestation_old; DROP INDEX IF EXISTS attestation_claim; DROP INDEX IF EXISTS attestation_witness; END IF;
  IF has_con THEN
    ALTER EXTENSION laplace DROP TABLE consensus; ALTER TABLE consensus RENAME TO consensus_old; END IF;
  CREATE TABLE IF NOT EXISTS attestation (claim blake3 NOT NULL, witness blake3 NOT NULL, score real NOT NULL, position integer) PARTITION BY RANGE (claim);
  CREATE TABLE IF NOT EXISTS consensus (claim blake3 NOT NULL, rating double precision NOT NULL, deviation double precision NOT NULL, volatility double precision NOT NULL, matches integer NOT NULL, PRIMARY KEY (claim)) PARTITION BY RANGE (claim);
  FOR k IN 0..15 LOOP
    lo := CASE WHEN k = 0 THEN 'MINVALUE' ELSE quote_literal(to_hex(k) || repeat('0', 31)) END;
    hi := CASE WHEN k = 15 THEN 'MAXVALUE' ELSE quote_literal(to_hex(k + 1) || repeat('0', 31)) END;
    EXECUTE format('CREATE TABLE IF NOT EXISTS attestation_%s PARTITION OF attestation FOR VALUES FROM (%s) TO (%s)', to_hex(k), lo, hi);
    EXECUTE format('CREATE TABLE IF NOT EXISTS consensus_%s PARTITION OF consensus FOR VALUES FROM (%s) TO (%s) WITH (fillfactor = 80)', to_hex(k), lo, hi);
  END LOOP;
  IF has_att THEN INSERT INTO attestation SELECT claim, witness, score, position FROM attestation_old; DROP TABLE attestation_old; END IF;
  IF has_con THEN INSERT INTO consensus SELECT claim, rating, deviation, volatility, matches FROM consensus_old; DROP TABLE consensus_old; END IF;
END $$;
SELECT laplace_schema_indexes();
