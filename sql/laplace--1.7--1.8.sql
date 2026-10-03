-- The container index takes a load's entries into its pending list and merges them once, when the load is done
-- (laplace ingest merges every GIN at the end of a source). At the default 4 MB the list filled every few thousand
-- paths and merged into the index's pages at random: each merge after a checkpoint wrote every page it touched into
-- the log whole, and the container index was 45% of a load's full-page images. 256 MB a partition: entries append in
-- order, and the index's own pages are written once a load.
DO $$
DECLARE r record;
BEGIN
  FOR r IN SELECT c.oid::regclass AS i FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname = 'gin' AND c.relkind = 'i'
           AND c.relname LIKE 'physicality%' LOOP
    EXECUTE format('ALTER INDEX %s SET (gin_pending_list_limit = 262144)', r.i);
  END LOOP;
END $$;
-- The packed mask's bit functions give way to the banks'.
DROP FUNCTION IF EXISTS laplace_mask_bit(text);
DROP FUNCTION IF EXISTS laplace_mask_bit(blake3);
-- The banks (manifest/banks.tsv): one mask per semantic group, on the row its group describes. A value's bit in its bank
-- is its frozen slot; laplace_bank_bit('kind', 'claim') is a row's kind, laplace_bank_bit('upos', 'NOUN') a part of speech.
CREATE FUNCTION laplace_bank_bit(bank text, value text) RETURNS smallint AS 'MODULE_PATHNAME', 'laplace_bank_bit' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION laplace_bank_of(blake3, OUT bank text, OUT grp text, OUT carrier text, OUT bit smallint) RETURNS record AS 'MODULE_PATHNAME', 'laplace_bank_of' LANGUAGE C STABLE STRICT PARALLEL SAFE;
