\echo Use "ALTER EXTENSION laplace UPDATE TO '1.36'" to load this file. \quit

-- Ingestion maintains GIN postings without acquiring permission to truncate
-- canonical content. Check the invoker's existing write authority at the
-- partition or its parent, even while executing with the extension owner's role.
CREATE OR REPLACE FUNCTION public.laplace_clean_pending_list(index_id pg_catalog.regclass)
RETURNS bigint LANGUAGE plpgsql VOLATILE STRICT SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE table_id oid; parent_id oid; caller name;
BEGIN
  SELECT i.indrelid INTO table_id
    FROM pg_index i
    JOIN pg_class idx ON idx.oid=i.indexrelid
    JOIN pg_am am ON am.oid=idx.relam
    JOIN pg_class tab ON tab.oid=i.indrelid
    JOIN pg_namespace ns ON ns.oid=tab.relnamespace
    JOIN pg_depend dep ON dep.classid='pg_class'::regclass
      AND dep.objid=tab.oid AND dep.deptype='e'
    JOIN pg_extension ext ON ext.oid=dep.refobjid
    WHERE i.indexrelid=index_id AND am.amname='gin'
      AND ns.nspname='public' AND tab.relname ~ '^physicality_[0-9a-f]{2}$'
      AND tab.relispartition AND ext.extname='laplace';
  IF NOT FOUND THEN
    RAISE EXCEPTION 'index is not an extension-owned Laplace physicality GIN index'
      USING ERRCODE='insufficient_privilege';
  END IF;
  parent_id := pg_partition_root(table_id);
  caller := nullif(current_setting('role'),'none');
  IF caller IS NULL THEN caller := session_user; END IF;
  IF NOT (has_table_privilege(caller,table_id,'INSERT') OR has_table_privilege(caller,parent_id,'INSERT'))
     OR NOT (has_table_privilege(caller,table_id,'UPDATE') OR has_table_privilege(caller,parent_id,'UPDATE')) THEN
    RAISE EXCEPTION 'Laplace ingestion write privileges are required'
      USING ERRCODE='insufficient_privilege';
  END IF;
  RETURN pg_catalog.gin_clean_pending_list(index_id);
END $$;
GRANT EXECUTE ON FUNCTION public.laplace_clean_pending_list(pg_catalog.regclass) TO PUBLIC;
COMMENT ON FUNCTION public.laplace_clean_pending_list(pg_catalog.regclass) IS
  'Clean only an extension-owned physicality GIN index for an invoker with INSERT and UPDATE on its partition or parent; does not require canonical-table TRUNCATE or index ownership.';
