-- Laplace semantics schema.
-- A claim is a composition: an entity with a physicality path of the entities it relates, hashed like any path, so it
-- lives in entity and physicality with all other content and GIN finds every claim that touches an entity. What sits
-- here is only what is not content: who witnessed, how much they are trusted, every attestation in the order it was
-- read, and the consensus on each claim: its Glicko-2 rating after its matchups.
CREATE TABLE IF NOT EXISTS witness (
  id       blake3 PRIMARY KEY,                     -- an entity: whatever testifies, named as content
  lineage  blake3,                                 -- the witness it derives from, so copies are not independent
  trust    double precision NOT NULL             -- -1 .. 1: MANDATE is 1, no information 0, reliably wrong -1
);
CREATE TABLE IF NOT EXISTS attestation (         -- the ledger: append-only, in reading order
  claim    blake3 NOT NULL,
  witness  blake3 NOT NULL,
  score    real NOT NULL                         -- win 1, draw 0.5, loss 0, or a score between
);
CREATE TABLE IF NOT EXISTS consensus (           -- updated in place as matchups are played
  claim       blake3 PRIMARY KEY,
  rating      double precision NOT NULL,
  deviation   double precision NOT NULL,
  volatility  double precision NOT NULL,
  matches     integer NOT NULL
) WITH (fillfactor = 80);
-- The position a witness gave a claim among its like (the order it lists a word's senses in), recorded as given.
ALTER TABLE attestation ADD COLUMN IF NOT EXISTS position integer;
