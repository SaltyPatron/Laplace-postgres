\set ECHO none
\pset format unaligned
\pset tuples_only on
SET client_min_messages = warning;
-- The deployed database is shared: every statement runs in this rolled-back
-- transaction, and no extension object is dropped, replaced or re-indexed.
BEGIN;
-- The deployed assertion index is attached and valid on every canonical
-- partition, so a reader can nominate assertion parents.
DO $$ BEGIN
 IF (SELECT count(*) FROM pg_index i JOIN pg_inherits h ON h.inhrelid=i.indexrelid
     WHERE h.inhparent='public.physicality_assertions'::regclass AND i.indisvalid AND i.indisready)<>256 THEN
  RAISE EXCEPTION 'the deployed assertion index is not valid on every canonical partition';
 END IF;
 PERFORM count(*) FROM public.laplace_testimony(ARRAY[public.laplace_id('index repair probe')]);
END $$;
-- Every declared C export is installed with its declared contract; repair
-- of an intact installation is idempotent and changes no function identity.
DO $$ DECLARE repaired oid:='public.laplace_testimony(public.blake3[],public.blake3[])'::regprocedure; BEGIN
  PERFORM public.laplace_restore_function_bindings();
  IF NOT EXISTS(SELECT FROM pg_depend d JOIN pg_extension e ON e.oid=d.refobjid
      WHERE d.classid='pg_proc'::regclass AND d.objid=repaired
        AND d.refclassid='pg_extension'::regclass AND d.deptype='e' AND e.extname='laplace')
      OR NOT EXISTS(SELECT FROM pg_proc WHERE oid=repaired AND pronargdefaults=1
        AND provolatile='s' AND proparallel='s' AND proretset) THEN
    RAISE EXCEPTION 'declared Native API is not installed with its declared contract';
  END IF;
  PERFORM public.laplace_restore_function_bindings();
  IF repaired <> 'public.laplace_testimony(public.blake3[],public.blake3[])'::regprocedure THEN
    RAISE EXCEPTION 'Native API repair changed an existing function identity';
  END IF;
END $$;
DO $$ BEGIN
  -- Remove per-call scope from both a frozen-bank primitive and an already
  -- parsed SQL adapter. Native SPI readers bind installed objects explicitly.
  -- These settings exist only inside this rolled-back transaction.
  ALTER FUNCTION public.laplace_mask_has_value(bit,text,text) SET search_path=public,pg_temp;
  ALTER FUNCTION public.laplace_coord(text) SET search_path=public,pg_temp;
  PERFORM public.laplace_restore_function_bindings();
  IF EXISTS (
    SELECT FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
    JOIN pg_depend d ON d.objid=p.oid AND d.classid='pg_proc'::regclass
    JOIN pg_extension e ON e.oid=d.refobjid AND d.refclassid='pg_extension'::regclass
    JOIN public.laplace_function_bindings() c ON c.symbol=p.prosrc
    WHERE e.extname='laplace' AND d.deptype='e'
      AND EXISTS(SELECT FROM unnest(p.proconfig) setting WHERE setting LIKE 'search_path=%')
  ) THEN RAISE EXCEPTION 'Native function binding contract was not restored'; END IF;
  IF EXISTS (
    SELECT FROM pg_proc p JOIN pg_language l ON l.oid=p.prolang
    JOIN pg_depend d ON d.objid=p.oid AND d.classid='pg_proc'::regclass
    JOIN pg_extension e ON e.oid=d.refobjid AND d.refclassid='pg_extension'::regclass
    WHERE e.extname='laplace' AND d.deptype='e' AND l.lanname='sql'
      AND (p.prosqlbody IS NULL OR EXISTS(SELECT FROM unnest(p.proconfig) s WHERE s LIKE 'search_path=%'))
  ) THEN RAISE EXCEPTION 'SQL adapters still bind names or set scope at each call'; END IF;
END $$;
SET LOCAL search_path=pg_catalog,pg_temp;
DO $$ BEGIN
  IF public.ST_AsEWKB(public.laplace_coord('A')) IS DISTINCT FROM public.laplace_coord_ewkb('A')
     OR NOT public.laplace_mask_has_value(public.laplace_mask_slot('kind','claim'),'kind','claim') THEN
    RAISE EXCEPTION 'Native adapter depends on caller search_path';
  END IF;
END $$;
ROLLBACK;
