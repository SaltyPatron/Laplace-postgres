-- Keep GIN maintenance inside the extension's authority. Runtime ingestion may
-- maintain its own physicality indexes without owning or altering their tables.
\echo Use "ALTER EXTENSION laplace UPDATE TO '1.13'" to load this file. \quit

CREATE FUNCTION public.laplace_clean_pending_list(index_id pg_catalog.regclass)
RETURNS bigint LANGUAGE plpgsql VOLATILE STRICT SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE table_id oid; caller name;
BEGIN
  SELECT i.indrelid INTO table_id
    FROM pg_index i
    JOIN pg_class idx ON idx.oid = i.indexrelid
    JOIN pg_am am ON am.oid = idx.relam
    JOIN pg_class tab ON tab.oid = i.indrelid
    JOIN pg_namespace ns ON ns.oid = tab.relnamespace
    JOIN pg_depend dep ON dep.classid = 'pg_class'::regclass
      AND dep.objid = tab.oid AND dep.deptype = 'e'
    JOIN pg_extension ext ON ext.oid = dep.refobjid
    WHERE i.indexrelid = index_id AND am.amname = 'gin'
      AND ns.nspname = 'public' AND tab.relname ~ '^physicality_[0-9a-f]{2}$'
      AND tab.relispartition AND ext.extname = 'laplace';
  IF NOT FOUND THEN
    RAISE EXCEPTION 'index is not an extension-owned Laplace physicality GIN index'
      USING ERRCODE = 'insufficient_privilege';
  END IF;
  caller := nullif(current_setting('role'), 'none');
  IF caller IS NULL THEN caller := session_user; END IF;
  IF NOT has_table_privilege(caller, table_id, 'INSERT')
     OR NOT has_table_privilege(caller, table_id, 'TRUNCATE') THEN
    RAISE EXCEPTION 'Laplace ingestion privileges are required'
      USING ERRCODE = 'insufficient_privilege';
  END IF;
  RETURN pg_catalog.gin_clean_pending_list(index_id);
END $$;
REVOKE ALL ON FUNCTION public.laplace_clean_pending_list(pg_catalog.regclass) FROM PUBLIC;

CREATE OR REPLACE FUNCTION laplace_attested(claims blake3[],
  OUT claim blake3, OUT trunk blake3, OUT games double precision, OUT tokens double precision, OUT score double precision,
  OUT "position" integer, OUT trust double precision, OUT witness boolean) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_attested' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 256;
CREATE OR REPLACE FUNCTION laplace_attribution(claims blake3[],
  OUT claim blake3, OUT trunk blake3, OUT games double precision, OUT tokens double precision, OUT score double precision,
  OUT trust double precision, OUT share_bp integer) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_attribution' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 256;

