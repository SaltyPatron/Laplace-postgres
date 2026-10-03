-- A claim is a game series: games plus a score (INVENTION.md §5). Each (claim, witness) is one attestation, its games
-- how many times the witness attested it and its score the mean over them; a repeat adds a game, never a row.
ALTER TABLE attestation ADD COLUMN IF NOT EXISTS games integer NOT NULL DEFAULT 1;
