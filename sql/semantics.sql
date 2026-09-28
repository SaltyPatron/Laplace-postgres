-- Laplace semantics schema (experimental, for measuring; not specification).
-- Claims are tuples of entities with their own content IDs. Every attestation is kept in an append-only ledger; a
-- claim's standing is maintained as attestations arrive. Observations (counts) and a witness's own order of senses sit
-- beside a claim, never inside its standing.
CREATE TABLE IF NOT EXISTS claim (
  id uuid PRIMARY KEY, subject uuid NOT NULL, predicate uuid NOT NULL, object uuid NOT NULL);
CREATE INDEX IF NOT EXISTS claim_subject ON claim (subject, predicate);
CREATE INDEX IF NOT EXISTS claim_object ON claim (object, predicate);

CREATE TABLE IF NOT EXISTS witness (                       -- a witness is an entity (its name is content)
  id uuid PRIMARY KEY, name text NOT NULL, lineage uuid NOT NULL, trust real NOT NULL, kind text NOT NULL);

CREATE TABLE IF NOT EXISTS attestation (                   -- the ledger: every matchup, in order, never updated
  seq bigint GENERATED ALWAYS AS IDENTITY, claim uuid NOT NULL, witness uuid NOT NULL,
  condition uuid,                                          -- an entity naming the conditions (a model component, a template)
  score real NOT NULL, z real) ;
CREATE INDEX IF NOT EXISTS attestation_claim ON attestation (claim);
CREATE INDEX IF NOT EXISTS attestation_witness ON attestation (witness);

CREATE TABLE IF NOT EXISTS standing (
  claim uuid PRIMARY KEY, rating real NOT NULL, deviation real NOT NULL, volatility real NOT NULL,
  matches int NOT NULL, witnesses uuid[] NOT NULL) WITH (fillfactor = 80);
CREATE TABLE IF NOT EXISTS occurrence (claim uuid NOT NULL, witness uuid NOT NULL, count bigint NOT NULL, PRIMARY KEY (claim, witness));
CREATE TABLE IF NOT EXISTS ordinal (claim uuid NOT NULL, witness uuid NOT NULL, position int NOT NULL, PRIMARY KEY (claim, witness));
