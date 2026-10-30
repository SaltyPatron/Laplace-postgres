\set ECHO none
\pset format unaligned
\pset tuples_only on
-- An upgrade must expose every declared C export, including APIs that were
-- missing from an installed predecessor. Repair is idempotent and never adopts
-- or overwrites a same-signature function owned by the host application.
DO $$ DECLARE repaired oid; BEGIN
  ALTER EXTENSION laplace DROP FUNCTION public.laplace_testimony(public.blake3[],public.blake3[]);
  DROP FUNCTION public.laplace_testimony(public.blake3[],public.blake3[]);
  PERFORM public.laplace_restore_function_bindings();
  repaired := 'public.laplace_testimony(public.blake3[],public.blake3[])'::regprocedure;
  IF NOT EXISTS(SELECT FROM pg_depend d JOIN pg_extension e ON e.oid=d.refobjid
      WHERE d.classid='pg_proc'::regclass AND d.objid=repaired
        AND d.refclassid='pg_extension'::regclass AND d.deptype='e' AND e.extname='laplace')
      OR NOT EXISTS(SELECT FROM pg_proc WHERE oid=repaired AND pronargdefaults=1
        AND provolatile='s' AND proparallel='s' AND proretset) THEN
    RAISE EXCEPTION 'missing Native API was not repaired with its declared contract';
  END IF;
  PERFORM public.laplace_restore_function_bindings();
  IF repaired <> 'public.laplace_testimony(public.blake3[],public.blake3[])'::regprocedure THEN
    RAISE EXCEPTION 'Native API repair changed an existing function identity';
  END IF;
  ALTER EXTENSION laplace DROP FUNCTION public.laplace_testimony(public.blake3[],public.blake3[]);
  DROP FUNCTION public.laplace_testimony(public.blake3[],public.blake3[]);
  CREATE FUNCTION public.laplace_testimony(public.blake3[],public.blake3[]) RETURNS integer
    LANGUAGE sql IMMUTABLE RETURN 7;
  BEGIN
    PERFORM public.laplace_restore_function_bindings();
    RAISE EXCEPTION 'Native API repair adopted a foreign function';
  EXCEPTION WHEN duplicate_object THEN NULL; END;
  IF public.laplace_testimony(NULL,NULL)<>7 THEN
    RAISE EXCEPTION 'Native API repair overwrote a foreign function';
  END IF;
  DROP FUNCTION public.laplace_testimony(public.blake3[],public.blake3[]);
  PERFORM public.laplace_restore_function_bindings();
  ALTER EXTENSION laplace DROP FUNCTION public.laplace_isa();
  BEGIN
    PERFORM public.laplace_restore_function_bindings();
    RAISE EXCEPTION 'Native binding verification ignored a missing declared export';
  EXCEPTION WHEN undefined_function THEN
    IF SQLERRM <> 'Native API is missing its declared function: laplace_isa' THEN RAISE; END IF;
  END;
  ALTER EXTENSION laplace ADD FUNCTION public.laplace_isa();
  PERFORM public.laplace_restore_function_bindings();
END $$;
DO $$ BEGIN
  -- Remove per-call scope from both a frozen-bank primitive and an already
  -- parsed SQL adapter. Native SPI readers bind installed objects explicitly.
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
-- A loaded predecessor and its callers can remain alive during an in-place
-- update. Rebinding must preserve the function identity and all other metadata.
DO $$ DECLARE module text; before_oid oid; before_meta jsonb; BEGIN
 SELECT probin,oid,to_jsonb(p)-'probin'-'proconfig' INTO module,before_oid,before_meta
 FROM pg_proc p WHERE oid='public.laplace_id(text)'::regprocedure;
 EXECUTE format('CREATE OR REPLACE FUNCTION public.laplace_id(text) RETURNS public.blake3 AS %L,%L LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE',module||'_peer','laplace_id');
 PERFORM public.laplace_id('loaded predecessor');
 PERFORM public.laplace_restore_function_bindings();
 IF NOT EXISTS(SELECT FROM pg_proc p WHERE oid=before_oid AND probin=module
     AND to_jsonb(p)-'probin'-'proconfig'=before_meta) THEN
   RAISE EXCEPTION 'module activation changed a function identity or attributes';
 END IF;
 IF public.laplace_id('loaded predecessor') IS NULL THEN
   RAISE EXCEPTION 'rebound function failed in a backend that loaded its predecessor';
 END IF;
END $$;
SET search_path=pg_catalog,pg_temp;
DO $$ BEGIN
  IF public.ST_AsEWKB(public.laplace_coord('A')) IS DISTINCT FROM public.laplace_coord_ewkb('A')
     OR NOT public.laplace_mask_has_value(public.laplace_mask_slot('kind','claim'),'kind','claim') THEN
    RAISE EXCEPTION 'Native adapter depends on caller search_path';
  END IF;
END $$;
RESET search_path;
DO $$ DECLARE module text; BEGIN
 SELECT probin||'_peer' INTO module FROM pg_proc WHERE oid='public.laplace_cp_id(integer)'::regprocedure;
 EXECUTE format('CREATE FUNCTION pg_temp.peer_id(text) RETURNS public.blake3 AS %L,%L LANGUAGE C IMMUTABLE STRICT',module,'laplace_id');
 EXECUTE format('CREATE FUNCTION pg_temp.peer_separators() RETURNS public.blake3[] AS %L,%L LANGUAGE C IMMUTABLE',module,'laplace_separator_ids');
 IF pg_temp.peer_id('ab')<>public.laplace_id('ab') OR pg_temp.peer_separators()<>public.laplace_separator_ids() THEN
  RAISE EXCEPTION 'loaded module generations disagree on backend-owned perf-cache configuration';
 END IF;
END $$;
